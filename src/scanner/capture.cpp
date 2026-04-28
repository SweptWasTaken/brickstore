// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#include <QCamera>
#include <QMediaDevices>
#include <QImageCapture>
#include <QMediaCaptureSession>
#include <QDeadlineTimer>
#include <QTimer>
#include <QGuiApplication>
#include <QPointer>
#include <QElapsedTimer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0) && QT_CONFIG(permissions)
#  include <QCoreApplication>
#  include <QPermissions>
#  include <QPointer>
#  if defined(BS_DESKTOP)
#    include <QMessageBox>
#  endif
#endif

#include "bricklink/core.h"
#include "bricklink/itemtype.h"
#include "common/eventfilter.h"
#include "core.h"
#include "capture.h"

#ifdef Q_OS_WIN
#  include <dshow.h>
#  include <wrl/client.h>
#endif


using namespace std::chrono_literals;

namespace Scanner {

class CapturePrivate
{
public:
    QMediaCaptureSession *captureSession = nullptr;
    QImageCapture *imageCapture = nullptr;
    QByteArray currentCameraId;
    std::unique_ptr<QCamera> camera;
    std::optional<int> currentCaptureId;
    QDeadlineTimer tryCaptureBefore;
    uint currentScanId = 0;
    QElapsedTimer currentScanTime;
    int averageScanTime = 1500;
    QByteArray currentBackendId;
    int progress = 0;
    Capture::State state = Capture::State::Idle;
    QPointer<EventFilter> windowTracker;
    bool appActive = true;
    bool winVisible = false;

    QString lastError;

    QTimer noMatchMessageTimeout;
    QTimer errorMessageTimeout;
    QTimer progressTimer;

    QList<const BrickLink::ItemType *> supportedFilters;
    const BrickLink::ItemType *currentFilter = nullptr;

#ifdef Q_OS_WIN
    Microsoft::WRL::ComPtr<IAMCameraControl> dshowCameraControl;
    long dshowZoomMin = 0, dshowZoomMax = 0, dshowZoomStep = 1, dshowZoomDefault = 0;
    bool hasDshowZoom = false;
#endif

    static bool s_hasCameraPermission;
};

bool CapturePrivate::s_hasCameraPermission = false;

#ifdef Q_OS_WIN
static Microsoft::WRL::ComPtr<IAMCameraControl> findDirectShowCameraControl(const QByteArray &deviceId)
{
    using Microsoft::WRL::ComPtr;

    // Qt's WMF backend and DirectShow register the same device under different interface class
    // GUIDs (KSCATEGORY_CAPTURE vs KSCATEGORY_VIDEO). Strip the "#{...}\suffix" portion and
    // compare only the device instance path, which is the same for both.
    auto instancePath = [](const QString &path) -> QString {
        int idx = path.indexOf(u"#{");
        return idx >= 0 ? path.left(idx) : path;
    };
    const QString targetInstance = instancePath(QString::fromLatin1(deviceId).toLower());

    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&devEnum)))) {
        qCWarning(LogScanner) << "DirectShow zoom: CoCreateInstance(ICreateDevEnum) failed";
        return {};
    }

    ComPtr<IEnumMoniker> enumMoniker;
    if (FAILED(devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0))
            || !enumMoniker) {
        qCWarning(LogScanner) << "DirectShow zoom: CreateClassEnumerator failed or no video input devices found";
        return {};
    }

    ComPtr<IMoniker> moniker;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        bool matched = false;
        ComPtr<IPropertyBag> propBag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag)))) {
            VARIANT var;
            VariantInit(&var);
            if (SUCCEEDED(propBag->Read(L"DevicePath", &var, nullptr))) {
                matched = (instancePath(QString::fromWCharArray(var.bstrVal).toLower()) == targetInstance);
                VariantClear(&var);
            }
        }
        if (matched) {
            ComPtr<IBaseFilter> filter;
            if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter)))) {
                qCWarning(LogScanner) << "DirectShow zoom: BindToObject failed — camera may be exclusively locked by another session";
                break;
            }
            ComPtr<IAMCameraControl> camControl;
            if (FAILED(filter.As(&camControl))) {
                qCWarning(LogScanner) << "DirectShow zoom: IAMCameraControl not supported by this camera's DirectShow filter";
                break;
            }
            return camControl;
        }
        moniker.Reset();
    }

    qCWarning(LogScanner) << "DirectShow zoom: no matching device found";
    return {};
}
#endif


Capture::Capture(QObject *parent)
    : QObject(parent)
    , d(new CapturePrivate)
{
    d->captureSession = new QMediaCaptureSession(this);
    d->imageCapture = new QImageCapture(d->captureSession);
    d->captureSession->setImageCapture(d->imageCapture);

    connect(d->captureSession, &QMediaCaptureSession::videoOutputChanged,
            this, &Capture::videoOutputChanged);

    d->errorMessageTimeout.setInterval(10s);
    d->errorMessageTimeout.setSingleShot(true);
    connect(&d->errorMessageTimeout, &QTimer::timeout, this, [this]() {
        if (state() == State::Error)
            setState(State::Idle);
    });
    d->noMatchMessageTimeout.setInterval(10s);
    d->noMatchMessageTimeout.setSingleShot(true);
    connect(&d->noMatchMessageTimeout, &QTimer::timeout, this, [this]() {
        if (state() == State::NoMatch)
            setState(State::Idle);
    });

    d->progressTimer.setInterval(30ms);
    connect(&d->progressTimer, &QTimer::timeout, this, [this]() {
        if ((state() == State::Scanning) && d->averageScanTime) {
            d->progress = std::clamp(int(100 * d->currentScanTime.elapsed() / d->averageScanTime), 0, 100);
            emit progressChanged(d->progress);
        }
    });

    connect(core(), &Core::scanFinished,
            this, [this](uint scanId, const QVector<Core::Result> &itemsAndScores) {
        if (scanId == d->currentScanId) {
            d->currentScanId = 0;
            d->lastError.clear();

            auto elapsed = int(d->currentScanTime.elapsed());
            if (!d->averageScanTime)
                d->averageScanTime = 1;
            d->averageScanTime = std::max(1, (d->averageScanTime + elapsed) / 2);

            QVector<const BrickLink::Item *> items;
            items.reserve(itemsAndScores.size());
            for (const auto &is : itemsAndScores)
                items << is.item;

            if (items.isEmpty()) {
                setState(State::NoMatch);
            } else {
                setState(State::Idle);
                emit captureAndScanFinished(items);
            }
        }
    });

    connect(core(), &Core::scanFailed,
            this, [this](uint scanId, const QString &error) {
        if (scanId == d->currentScanId) {
            d->currentScanId = 0;
            d->lastError = error;
            setState(State::Error);
        }
    });

    connect(d->imageCapture, &QImageCapture::errorOccurred,
            this, [this](int id, QImageCapture::Error error, const QString &errorString) {
        Q_UNUSED(error)

        if (!d->currentCaptureId.has_value() || d->currentCaptureId.value() != id) {
            qCCritical(LogScanner) << "Ignoring errorOccurred(id:" << id << "), current:"
                                   << d->currentCaptureId.value_or(-1);
        }
        d->currentCaptureId.reset();
        d->lastError = errorString;
        setState(State::Error);
    });

    connect(d->imageCapture, &QImageCapture::readyForCaptureChanged,
            this, [this](bool ready) {
        if (ready && !d->tryCaptureBefore.hasExpired())
            captureAndScan();
    });

    connect(d->imageCapture, &QImageCapture::imageCaptured,
            this, [this](int id, const QImage &img) {
        if (!d->currentCaptureId.has_value() || d->currentCaptureId.value() != id) {
            qCCritical(LogScanner) << "Ignoring imageCaptured(id:" << id << "), current:"
                                   << d->currentCaptureId.value_or(-1);
        }
        d->currentCaptureId.reset();
        d->currentScanId = core()->scan(img, d->currentFilter, d->currentBackendId);

        if (!d->currentScanId) {
            d->lastError = tr("Scanning failed");
            setState(State::Error);
        } else {
            setState(State::Scanning);
        }
    });

    setCurrentBackendId(core()->defaultBackendId());
    setCurrentCameraId(QMediaDevices::defaultVideoInput().id());

    connect(qApp, &QGuiApplication::applicationStateChanged,
            this, [this](Qt::ApplicationState appState) {
        if (appState == Qt::ApplicationInactive) {
            d->appActive = false;
        } else if (appState == Qt::ApplicationActive) {
            d->appActive = true;
            if (d->winVisible && (state() == State::Inactive))
                setState(State::Idle);
        }
    });
}

QObject *Capture::videoOutput() const
{
    return d->captureSession->videoOutput();
}

void Capture::setVideoOutput(QObject *videoOutput)
{
    d->captureSession->setVideoOutput(videoOutput);
}

void Capture::trackWindowVisibility(QObject *window)
{
    delete d->windowTracker;
    d->winVisible = false;
    if (!window)
        return;

    d->windowTracker = new EventFilter(window, { QEvent::Hide, QEvent::Show },
                                       [this](QObject *, QEvent *e) {
        if (e->type() == QEvent::Hide) {
            d->winVisible = false;
            if (state() != State::Inactive)
                setState(State::Inactive);
        } else if (e->type() == QEvent::Show) {
            d->winVisible = true;
            if (d->appActive && (state() == State::Inactive))
                setState(State::Idle);
        }
        return EventFilter::ContinueEventProcessing;
    });
}

void Capture::captureAndScan()
{
    if ((state() != State::Scanning) && (state() != State::Capturing)) {
        if (d->imageCapture->isReadyForCapture()) {
            setState(State::Capturing);
            d->currentCaptureId = d->imageCapture->capture();
        } else {
            // The cam might not be ready yet (e.g. window activation). Wait for it to become ready
            // and try again.
            d->tryCaptureBefore = QDeadlineTimer(1000);
        }
    }
}

void Capture::checkSystemPermissions(QObject *context, const std::function<void(bool)> &callback)
{
    if (CapturePrivate::s_hasCameraPermission) {
        if (callback)
            callback(true);
        return;
    }

    const QString requestDenied = tr("BrickStore's request for camera access was denied. You will not be able to use your webcam to identify parts until you grant the required permissions via your system's Settings application.");

#if (QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)) && QT_CONFIG(permissions)
    QCameraPermission cameraPermission;
    switch (qApp->checkPermission(cameraPermission)) {
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(cameraPermission, context, [callback](const QPermission &p) {
            CapturePrivate::s_hasCameraPermission = (p.status() == Qt::PermissionStatus::Granted);
            if (callback)
                callback(CapturePrivate::s_hasCameraPermission);
        });
        return;
    case Qt::PermissionStatus::Denied:
#  if defined(BS_DESKTOP)
        QMessageBox::warning(nullptr, QCoreApplication::applicationName(), requestDenied);
#   endif
        if (callback)
            callback(false);
        return;
    case Qt::PermissionStatus::Granted:
        break; // Proceed
    }
#else
    Q_UNUSED(context)
    Q_UNUSED(requestDenied)
#endif
    CapturePrivate::s_hasCameraPermission = true;
    if (callback)
        callback(true);
    return;
}

Capture::State Capture::state() const
{
    return d->state;
}

void Capture::setState(State newState)
{
    qWarning() << "NEW STATE" << newState << "[[ OLD STATE" << d->state << "]]";

    switch (newState) {
    case State::Idle:
        if (d->camera && !d->camera->isActive())
            d->camera->start();
        break;
    case State::Inactive:
        if (d->camera && d->camera->isActive())
            d->camera->stop();
        break;
    case State::Capturing:
        d->currentScanTime.start();
        break;
    case State::Scanning:
        break;
    case State::NoMatch:
        d->noMatchMessageTimeout.start();
        break;
    case State::Error:
        d->errorMessageTimeout.start();
        break;
    }
    if (newState == d->state)
        return;

    if (newState == State::Scanning) {
        d->progressTimer.start();
    } else {
        d->progressTimer.stop();
        d->progress = 0;
        emit progressChanged(d->progress);
    }

    d->state = newState;
    emit stateChanged(newState);
}

QString Capture::lastError() const
{
    return d->lastError;
}

int Capture::progress() const
{
    return d->progress;
}

bool Capture::isCameraActive() const
{
    return d->camera ? d->camera->isActive() : false;
}

QByteArray Capture::currentCameraId() const
{
    return d->currentCameraId;
}

void Capture::setCurrentCameraId(const QByteArray &newCameraId)
{
    if (d->currentCameraId == newCameraId)
        return;

    QCameraDevice newCameraDevice;
    const auto allCameraDevices = QMediaDevices::videoInputs();
    for (const auto &cameraDevice : allCameraDevices) {
        if (cameraDevice.id() == newCameraId) {
            newCameraDevice = cameraDevice;
            break;
        }
    }
    if (newCameraDevice.isNull())
        return;

    d->currentCameraId = newCameraId;
    emit currentCameraIdChanged(newCameraId);

    d->camera = std::make_unique<QCamera>(newCameraDevice);
    connect(d->camera.get(), &QCamera::activeChanged,
            this, &Capture::cameraActiveChanged);
    connect(d->camera.get(), &QCamera::zoomFactorChanged, this, [this](qreal factor) {
        emit zoomValueChanged(qRound(factor * 10));
    });

    d->captureSession->setCamera(d->camera.get());

#ifdef Q_OS_WIN
    d->dshowCameraControl.Reset();
    d->hasDshowZoom = false;
    auto dsControl = findDirectShowCameraControl(newCameraId);
    if (dsControl) {
        long minVal, maxVal, step, defaultVal, flags;
        HRESULT hr = dsControl->GetRange(CameraControl_Zoom, &minVal, &maxVal, &step,
                                         &defaultVal, &flags);
        if (FAILED(hr)) {
            qCWarning(LogScanner) << "DirectShow zoom: IAMCameraControl::GetRange(CameraControl_Zoom) failed, hr=" << hr;
        } else if (maxVal <= minVal) {
            qCWarning(LogScanner) << "DirectShow zoom: GetRange succeeded but zoom range is empty (min=" << minVal << "max=" << maxVal << ")";
        } else {
            d->dshowCameraControl = dsControl;
            d->dshowZoomMin     = minVal;
            d->dshowZoomMax     = maxVal;
            d->dshowZoomStep    = qMax(1L, step);
            d->dshowZoomDefault = defaultVal;
            d->hasDshowZoom     = true;
            qCWarning(LogScanner) << "DirectShow zoom: ready — range" << minVal << "to" << maxVal << "step" << step;
            long curVal = defaultVal, curFlags = 0;
            if (SUCCEEDED(d->dshowCameraControl->Get(CameraControl_Zoom, &curVal, &curFlags)))
                qCWarning(LogScanner) << "DirectShow zoom: current zoom value =" << curVal;
            else
                qCWarning(LogScanner) << "DirectShow zoom: Get(CameraControl_Zoom) failed — no read access";
        }
    }
#endif

    d->camera->start();

    // Emit zoomRangeChanged on the next event loop turn so that any caller constructing
    // a dialog (and connecting to this signal afterwards) is guaranteed to receive it.
    QMetaObject::invokeMethod(this, [this]() {
        emit zoomRangeChanged(zoomMinimum(), zoomMaximum(), zoomStep());
    }, Qt::QueuedConnection);
}

QByteArray Capture::currentBackendId() const
{
    return d->currentBackendId;
}

void Capture::setCurrentBackendId(const QByteArray &backendId)
{
    if (d->currentBackendId == backendId)
        return;

    if (const auto *backend = core()->backendFromId(backendId)) {
        d->currentBackendId = backendId;

        auto oldFilters = d->supportedFilters;
        d->supportedFilters.clear();
        for (const char c : backend->itemTypeFilter) {
            if (const auto *itemType = BrickLink::core()->itemType(c))
                d->supportedFilters << itemType;
        }
        if (d->supportedFilters != oldFilters) {
            if (d->currentFilter && !d->supportedFilters.contains(d->currentFilter))
                setCurrentItemTypeFilter(nullptr);

            emit supportedItemTypeFiltersChanged(d->supportedFilters);
        }
        emit currentBackendIdChanged(backendId);
    }
}

const BrickLink::ItemType *Capture::currentItemTypeFilter() const
{
    return d->currentFilter;
}

void Capture::setCurrentItemTypeFilter(const BrickLink::ItemType *filter)
{
    if ((filter != d->currentFilter) && (!filter || d->supportedFilters.contains(filter))) {
        d->currentFilter = filter;
        emit currentItemTypeFilterChanged(filter);
    }
}

QList<const BrickLink::ItemType *> Capture::supportedItemTypeFilters() const
{
    return d->supportedFilters;
}

int Capture::zoomValue() const
{
#ifdef Q_OS_WIN
    if (d->hasDshowZoom) {
        long value, flags;
        if (SUCCEEDED(d->dshowCameraControl->Get(CameraControl_Zoom, &value, &flags)))
            return int(value);
        return int(d->dshowZoomMin);
    }
#endif
    return d->camera ? qRound(d->camera->zoomFactor() * 10) : 10;
}

void Capture::setZoomValue(int value)
{
#ifdef Q_OS_WIN
    if (d->hasDshowZoom) {
        d->dshowCameraControl->Set(CameraControl_Zoom, long(value), CameraControl_Flags_Manual);
        emit zoomValueChanged(value);
        return;
    }
#endif
    if (d->camera)
        d->camera->setZoomFactor(value / 10.0);
}

int Capture::zoomMinimum() const
{
#ifdef Q_OS_WIN
    if (d->hasDshowZoom)
        return int(d->dshowZoomMin);
#endif
    return d->camera ? qRound(d->camera->minimumZoomFactor() * 10) : 10;
}

int Capture::zoomMaximum() const
{
#ifdef Q_OS_WIN
    if (d->hasDshowZoom)
        return int(d->dshowZoomMax);
#endif
    return d->camera ? qRound(d->camera->maximumZoomFactor() * 10) : 10;
}

int Capture::zoomStep() const
{
#ifdef Q_OS_WIN
    if (d->hasDshowZoom)
        return int(d->dshowZoomStep);
#endif
    return 1;
}

} // namespace Scanner

#include "moc_capture.cpp"
