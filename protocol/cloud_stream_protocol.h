// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CLOUD_STREAM_PUBLIC_CLOUD_STREAM_PROTOCOL_H_
#define CHROME_BROWSER_CLOUD_STREAM_PUBLIC_CLOUD_STREAM_PROTOCOL_H_

// Wire protocol between a Chrome instance running with --cloud-stream-pipe
// and the streaming gateway. This header has no Chromium dependencies so the
// gateway can include the same file.
//
// Transport: one duplex named pipe per Chrome instance, created by the
// gateway (\\.\pipe\<name>), opened by Chrome. Every message is a
// MessageHeader followed by `payload_size` bytes. All integers are
// little-endian, all coordinates are in window pixels.

#include <stdint.h>

namespace cloud_stream {

inline constexpr uint32_t kProtocolVersion = 1;

enum class MessageType : uint32_t {
  // Gateway -> Chrome.
  kHello = 1,             // HelloMessage
  kMouse = 10,            // MouseMessage
  kWheel = 11,            // WheelMessage
  kKey = 12,              // KeyMessage
  kText = 13,             // TextMessage + UTF-16 text
  kComposition = 14,      // CompositionMessage + UTF-16 text
  kBeginFrame = 20,       // BeginFrameMessage
  kFrameDone = 21,        // FrameDoneMessage
  kSetWindowBounds = 22,  // WindowBoundsMessage
  kFileChosen = 30,       // FileChosenMessage + UTF-16 paths ('\0' separated)
  kClipboardText = 31,    // ClipboardMessage + UTF-16 text

  // Chrome -> gateway.
  kHelloAck = 100,          // HelloAckMessage
  kWindowCreated = 110,     // WindowMessage
  kWindowDestroyed = 111,   // WindowMessage
  kWindowBounds = 112,      // WindowBoundsMessage
  kFrame = 120,             // FrameMessage
  kCursor = 130,            // CursorMessage [+ BGRA pixels]
  kFileChooserRequest = 140,  // FileChooserRequestMessage + UTF-16 title
  kClipboardChanged = 141,  // ClipboardMessage + UTF-16 text
};

#pragma pack(push, 1)

struct MessageHeader {
  MessageType type;
  uint32_t payload_size;
};

// --- Gateway -> Chrome -----------------------------------------------------

struct HelloMessage {
  uint32_t protocol_version;
  // Gateway process id. Chrome duplicates frame handles into this process.
  uint32_t gateway_pid;
  // DXGI adapter LUID the gateway encodes on. Must match --use-adapter-luid.
  uint32_t adapter_luid_low;
  int32_t adapter_luid_high;
};

enum class MouseAction : uint32_t {
  kMove = 0,
  kDown = 1,
  kUp = 2,
  kEnter = 3,
  kLeave = 4,
};

// Keyboard modifier bit values match ui::EventFlags. Button bits are
// protocol-specific; Chrome maps them to ui::EF_*_MOUSE_BUTTON.
enum Modifier : uint32_t {
  kShift = 1 << 1,
  kControl = 1 << 2,
  kAlt = 1 << 3,
  kCommand = 1 << 4,  // Windows key.
  kLeftButton = 1 << 6,
  kMiddleButton = 1 << 7,
  kRightButton = 1 << 8,
};

struct MouseMessage {
  uint32_t window_id;
  MouseAction action;
  int32_t x;
  int32_t y;
  uint32_t modifiers;       // Modifier bits, including held buttons.
  uint32_t changed_button;  // kLeftButton/kMiddleButton/kRightButton or 0.
  uint32_t click_count;     // For kDown: 1, 2, 3...
};

struct WheelMessage {
  uint32_t window_id;
  int32_t x;
  int32_t y;
  int32_t delta_x;  // Multiples of 120 per notch, like WM_MOUSEWHEEL.
  int32_t delta_y;
  uint32_t modifiers;
};

struct KeyMessage {
  uint32_t window_id;
  uint32_t pressed;      // 1 = key down, 0 = key up.
  uint32_t windows_vk;   // Windows virtual-key code (VK_*).
  uint32_t scan_code;    // PS/2 set 1 scan code, extended keys | 0xE000.
  uint32_t modifiers;
};

// Committed text, typed characters or IME result. Followed by
// `length` UTF-16 code units.
struct TextMessage {
  uint32_t window_id;
  uint32_t length;
};

// In-progress IME composition. Followed by `length` UTF-16 code units.
// length == 0 clears the composition.
struct CompositionMessage {
  uint32_t window_id;
  uint32_t length;
  uint32_t cursor;  // Caret position inside the composition.
};

struct BeginFrameMessage {
  uint32_t window_id;  // 0 = all windows.
  int64_t frame_time_us;
  int64_t interval_us;
};

struct FrameDoneMessage {
  uint32_t window_id;
  uint64_t frame_seq;
};

struct WindowBoundsMessage {
  uint32_t window_id;
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

struct FileChosenMessage {
  uint32_t request_id;
  uint32_t canceled;  // 1 = user canceled.
  uint32_t length;    // UTF-16 code units of the path list that follows.
};

struct ClipboardMessage {
  uint32_t length;  // UTF-16 code units that follow.
};

// --- Chrome -> gateway -----------------------------------------------------

struct HelloAckMessage {
  uint32_t protocol_version;
  uint32_t chrome_pid;
};

enum class WindowKind : uint32_t {
  kNormal = 0,  // Browser window or app window.
  kPopup = 1,   // Menus, bubbles, <select>, tooltips, extension popups.
};

struct WindowMessage {
  uint32_t window_id;
  WindowKind kind;
  uint32_t parent_window_id;  // 0 if none.
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

struct FrameMessage {
  uint32_t window_id;
  uint64_t frame_seq;
  // NT handle to a D3D11 texture, already duplicated into the gateway
  // process. 0 if `texture_token` refers to a texture sent before; the
  // gateway caches opened textures by token.
  uint64_t texture_handle;
  uint64_t texture_token_high;
  uint64_t texture_token_low;
  uint32_t coded_width;
  uint32_t coded_height;
  int32_t visible_x;
  int32_t visible_y;
  uint32_t visible_width;
  uint32_t visible_height;
  int64_t timestamp_us;
  // The gateway must answer every frame with kFrameDone, otherwise the
  // capturer runs out of buffers.
};

struct CursorMessage {
  uint32_t window_id;
  uint32_t cursor_type;  // ui::mojom::CursorType value.
  // For custom cursors (cursor_type == kCustom): BGRA pixels follow.
  uint32_t width;
  uint32_t height;
  int32_t hotspot_x;
  int32_t hotspot_y;
};

enum class FileChooserMode : uint32_t {
  kOpen = 0,
  kOpenMultiple = 1,
  kFolder = 2,
  kSave = 3,
};

struct FileChooserRequestMessage {
  uint32_t request_id;
  uint32_t window_id;
  FileChooserMode mode;
  uint32_t title_length;  // UTF-16 code units that follow.
};

#pragma pack(pop)

}  // namespace cloud_stream

#endif  // CHROME_BROWSER_CLOUD_STREAM_PUBLIC_CLOUD_STREAM_PROTOCOL_H_
