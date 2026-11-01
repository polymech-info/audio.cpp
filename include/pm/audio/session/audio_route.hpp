#pragma once

// Runtime device selection. aaudio:<id> is valid for the current route list only.
// A saved profile stores a descriptor (type, product, address), never a raw id.
// Requesting an id does not mean that id was opened: verify_opened_device decides
// whether audio may be transmitted.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pm::audio::session {

enum class SelectorKind : uint8_t { Name, AaudioId };

struct DeviceSelector {
    bool ok = false;
    bool is_default = false;          // empty selector: system default
    SelectorKind kind = SelectorKind::Name;
    std::string name;                  // kind == Name, already trimmed
    int aaudio_id = -1;                // kind == AaudioId
    const char* error = "";
};

DeviceSelector parse_device_selector(std::string_view text);

struct DeviceDescriptor {
    std::string type;     // builtin, wired, bluetooth, usb, ... empty = any type
    std::string product;  // product name, compared case-insensitively
    std::string address;  // stable address when the platform has one
};

struct RouteCandidate {
    int id = -1;
    std::string type;
    std::string product;
    std::string address;
    bool output = false;  // communication-device APIs take an output
};

struct RouteMatch {
    bool ok = false;
    int id = -1;
    const char* reason = "";
};

// Address match wins and does not fall through to the product name.
// More than one match is a failure. An empty descriptor is a failure.
RouteMatch match_descriptor(const DeviceDescriptor& want,
                            const RouteCandidate* devices,
                            std::size_t count);

struct OpenVerdict {
    bool transmit = false;
    const char* reason = "";
};

// specific=false accepts whatever was opened, as long as the id is readable.
// specific=true transmits only when opened_id == requested_id.
OpenVerdict verify_opened_device(bool specific, int requested_id, int opened_id) noexcept;

enum class RouteLossAction : uint8_t { Stop };

struct RouteLoss {
    RouteLossAction action = RouteLossAction::Stop;
    const char* reason = "";
};

// A lost route always stops. There is no silent reopen on the default device.
RouteLoss on_route_lost(bool specific) noexcept;

struct CommChoice {
    bool ok = false;
    int output_id = -1;
    const char* reason = "";
};

// requested_id must be an output in `devices`. An input id is rejected:
// setCommunicationDevice takes an output and Android pairs the input.
CommChoice select_communication_output(int requested_id,
                                       const RouteCandidate* devices,
                                       std::size_t count);

} // namespace pm::audio::session
