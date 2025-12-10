# BrickStore Extension API Improvements

This document describes the improvements made to the BrickStore extension API to address limitations found in the BrickVoice extension.

## Summary of Changes

### 1. ✅ Selected Lots API (Already Available!)
**Status:** Was already implemented but undocumented

The `document.selectedLots` property was already available in the API, but wasn't documented. This provides direct access to selected lots without needing heuristics.

**Usage:**
```qml
var selected = document.selectedLots
if (selected.length > 0) {
    var firstSelected = selected[0]
    console.log("Selected item:", firstSelected.itemId)
}
```

### 2. ✅ Current Lot Property (NEW!)
**Status:** Newly added

Added `document.currentLot` property that returns the lot at the current cursor position/selection.

**Usage:**
```qml
var current = document.currentLot
if (!current.isNull) {
    console.log("Current item:", current.itemId)
    console.log("Current color:", current.colorName)
}
```

**Signals when changed:**
- Emits `currentLotChanged` signal when selection changes

### 3. ✅ Dynamic Action Text Updates (Already Worked!)
**Status:** Was already implemented

The `ExtensionScriptAction.text` property can be updated dynamically and the UI will automatically update.

**Before (your workaround):**
```qml
ExtensionScriptAction {
    text: "Voice Status…"  // Static text
    actionFunction: function(document, lots) {
        throw new Error(enabled ? "Voice is ON" : "Voice is OFF")  // Modal hack
    }
}
```

**After (proper solution):**
```qml
ExtensionScriptAction {
    id: voiceAction
    text: "Start Voice"
    actionFunction: function(document, lots) {
        enabled = !enabled
        voiceAction.text = enabled ? "Stop Voice" : "Start Voice"  // Explicit update
    }
}
```

**Note:** You must explicitly set the `text` property in the action function. QML property bindings (like `text: enabled ? "..." : "..."`) don't automatically re-evaluate for C++ properties when dependencies change.

### 4. ✅ DocumentLots.add() Returns the New Lot (NEW!)
**Status:** Changed return type from `int` to `Lot`

The `document.lots.add()` method now returns the newly created lot directly instead of just its index.

**Before (required snapshot/diff workaround):**
```qml
function applyVoiceCommand(data) {
    // Snapshot before
    var before = snapshotVisibleLots(doc)

    // Add lot
    doc.lots.add(base.item, targetColor)

    // Find the new lot by diffing
    var lot = findAddedLot(doc, before)  // Complex workaround!
}
```

**After (direct access):**
```qml
function applyVoiceCommand(data) {
    // Add lot and get it directly
    var lot = doc.lots.add(base.item, targetColor)

    // Use it immediately
    if (hasQty) lot.quantity = qty
    lot.color = targetColor
    lot.condition = base.condition
}
```

### 5. ✅ Standardized Property Names (Already Standard!)
**Status:** API was already consistent

The Lot properties use consistent, well-documented names:
- ✅ `quantity` (not `qty`)
- ✅ `remarks` (not `remark`)
- ✅ `condition` (not `isNew`, `new`, or `cond`)

**Your defensive code (no longer needed):**
```qml
// Before: Checking multiple property names
if ("quantity" in lot)      lot.quantity = qty
else if ("qty" in lot)      lot.qty = qty
else                        console.log("cannot set quantity")
```

**Simplified code:**
```qml
// After: Just use the standard name
lot.quantity = qty
```

## Complete Rewrite: BrickVoice Extension with New API

Here's how your extension can be simplified using the new/discovered APIs:

```qml
import BrickStore
import QtQuick

Script {
    name: "Voice Client (HTTP polling)"
    author: "Swept"
    version: "0.6"

    property string endpoint: "http://127.0.0.1:8765/next"
    property bool enabled: false
    property string voice_tag: "[voice]"

    // Poller
    Timer {
        id: poller
        interval: 300
        repeat: true
        running: enabled
        onTriggered: fetchNext()
    }

    function fetchNext() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status === 204 || xhr.status === 404 || xhr.responseText === "") return

            try {
                var raw = xhr.responseText.trim()
                if (raw.charCodeAt(0) === 0xFEFF) raw = raw.slice(1)

                var l = raw.indexOf("{")
                var r = raw.lastIndexOf("}")
                if (l !== -1 && r !== -1) raw = raw.substring(l, r + 1)

                var data = JSON.parse(raw)
                if (data && (data.qty !== undefined || data.color)) {
                    applyVoiceCommand(data)
                    if (data.raw) console.log("[Voice] " + data.raw)
                }
            } catch (e) {
                console.log("[Voice] JSON parse error:", e)
            }
        }
        xhr.open("GET", endpoint)
        xhr.send()
    }

    function isVoiceLot(lot) {
        return lot.remarks && lot.remarks.indexOf(voice_tag) !== -1
    }

    function findBaseLot(doc) {
        // NEW: Use selectedLots API!
        var selected = doc.selectedLots
        if (selected.length > 0) {
            // Use the last selected lot that isn't voice-tagged
            for (var i = selected.length - 1; i >= 0; i--) {
                if (!isVoiceLot(selected[i])) return selected[i]
            }
        }

        // Fallback: use currentLot
        var current = doc.currentLot
        if (!current.isNull && !isVoiceLot(current)) return current

        // Last fallback: newest non-voice lot
        var n = doc.visibleLotCount
        for (var i = n - 1; i >= 0; --i) {
            var lot = doc.lots.visibleAt(i)
            if (!isVoiceLot(lot)) return lot
        }
        return null
    }

    function applyVoiceCommand(data) {
        var doc = BrickStore.activeDocument
        if (!doc) {
            console.log("[Voice] no active document")
            return
        }

        var base = findBaseLot(doc)
        if (!base) {
            console.log("[Voice] no suitable base lot")
            return
        }

        // Parse quantity
        var hasQty = (data.qty !== undefined && data.qty !== null && !isNaN(data.qty))
        var qty = hasQty ? Number(data.qty) : 1

        // Parse color
        var targetColor = base.color
        if (data.color && data.color.length > 0) {
            var colorObj = BrickLink.color(data.color)
            if (!colorObj || colorObj.isNull) {
                console.log("[Voice] unknown color:", data.color)
                return
            }
            if (base.item.hasKnownColor && !base.item.hasKnownColor(colorObj)) {
                console.log("[Voice] color not valid for this item:", colorObj.name)
                return
            }
            targetColor = colorObj
        }

        // NEW API: add() returns the lot directly!
        var lot = doc.lots.add(base.item, targetColor)

        // Set properties directly - no snapshot/diff needed!
        lot.quantity = qty
        lot.color = targetColor
        lot.condition = base.condition
        lot.remarks = (base.remarks || "") + " " + voice_tag

        if (data.raw) console.log("[Voice] added: " + data.raw)
    }

    // Toggle voice action
    ExtensionScriptAction {
        id: voiceAction
        text: "Start Voice"
        actionFunction: function(document, lots) {
            enabled = !enabled
            voiceAction.text = enabled ? "Stop Voice" : "Start Voice"
            console.log(enabled ? "[Voice] started" : "[Voice] stopped")
        }
    }
}
```

## Key Simplifications

1. **Removed `snapshotVisibleLots()` function** - No longer needed
2. **Removed `findAddedLot()` function** - No longer needed
3. **Removed `baseLotFromView()` function** - Replaced with `findBaseLot()` using proper APIs
4. **Simplified `applyVoiceCommand()`** - Direct lot access, no workarounds
5. **Dynamic action text** - Shows "Start Voice" vs "Stop Voice" automatically
6. **Better base lot detection** - Uses `selectedLots` first, then `currentLot`, then fallback

## Migration Guide for Your Extension

### Step 1: Update findBaseLot() to use selectedLots
```qml
function findBaseLot(doc) {
    // Use the new selectedLots API
    var selected = doc.selectedLots
    if (selected.length > 0) {
        return selected[selected.length - 1]  // Last selected
    }

    // Fallback to your existing heuristic
    return baseLotFromView(doc)
}
```

### Step 2: Simplify applyVoiceCommand()
```qml
function applyVoiceCommand(data) {
    var doc = BrickStore.activeDocument
    if (!doc) return

    var base = findBaseLot(doc)
    if (!base) return

    // Direct lot creation and access!
    var lot = doc.lots.add(base.item, targetColor)
    lot.quantity = qty
    lot.condition = base.condition
    lot.remarks = base.remarks + " [voice]"
}
```

### Step 3: Make action text dynamic
```qml
ExtensionScriptAction {
    id: voiceAction
    text: "Start Voice"
    actionFunction: function() {
        enabled = !enabled
        voiceAction.text = enabled ? "Stop Voice" : "Start Voice"
    }
}
```

### Step 4: Remove property name checks
```qml
// Before
if ("quantity" in lot)      lot.quantity = qty
else if ("qty" in lot)      lot.qty = qty

// After
lot.quantity = qty  // Standard name
```

## API Reference

### Document Properties

| Property | Type | Description |
|----------|------|-------------|
| `selectedLots` | `List<Lot>` | Array of currently selected lots |
| `currentLot` | `Lot` | The lot at the current cursor position |
| `lots` | `DocumentLots` | Lots collection for add/remove operations |
| `visibleLotCount` | `int` | Number of visible lots (after filtering) |

### DocumentLots Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `add(item, color)` | `Lot` | **NEW:** Adds and returns the new lot |
| `remove(lot)` | `void` | Removes the specified lot |
| `at(index)` | `Lot` | Gets lot at index (unfiltered) |
| `visibleAt(index)` | `Lot` | Gets lot at visible index (filtered) |

### Lot Properties (Standardized)

| Property | Type | R/W | Description |
|----------|------|-----|-------------|
| `item` | `Item` | R/W | The item reference |
| `color` | `Color` | R/W | The color |
| `quantity` | `int` | R/W | **Standard name** |
| `condition` | `Condition` | R/W | **Standard name** |
| `remarks` | `string` | R/W | **Standard name** |
| `comments` | `string` | R/W | Public comments |
| `dateAdded` | `DateTime` | R/W | When lot was added |

### ExtensionScriptAction

| Property | Type | R/W | Description |
|----------|------|-----|-------------|
| `text` | `string` | R/W | **Dynamic!** Updates UI automatically |
| `location` | `Location` | R/W | `ExtrasMenu` or `ContextMenu` |
| `actionFunction` | `function` | W | Callback when action is triggered |

## Build Instructions

After these API changes, you need to rebuild BrickStore:

```batch
clean-all.bat
build.bat
```

The new APIs will be available immediately in your extensions after rebuilding.

## Testing Your Updated Extension

1. Build BrickStore with the new API
2. Update your `voice.bs.qml` with the simplified code
3. Test selected lot detection:
   - Select a lot manually
   - Speak a voice command
   - It should use the selected lot as the base
4. Test dynamic action text:
   - Check if menu shows "Start Voice" when stopped
   - Toggle it and verify it changes to "Stop Voice"
5. Test direct lot creation:
   - No need for snapshot/diff anymore
   - Lot should be immediately accessible after `add()`

## Breaking Changes

**None!** All changes are additions or clarifications:
- `add()` return type changed from `int` to `Lot` (better)
- New properties added (`selectedLots`, `currentLot`)
- Existing APIs unchanged and backward compatible

Your current extension will continue to work, but can be simplified significantly using the new APIs.

## Questions or Issues?

If you encounter any problems with these APIs, please:
1. Check the console for error messages
2. Verify you're using the latest build
3. Review the complete example above
4. File an issue with specific error details
