#include "pm/audio/session/audio_route.hpp"

#include <cctype>

namespace pm::audio::session {
namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

bool same_ci(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

bool parse_i32(std::string_view s, int& out)
{
    if (s.empty())
        return false;
    int value = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9')
            return false;
        const int digit = ch - '0';
        if (value > (2147483647 - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

} // namespace

DeviceSelector parse_device_selector(std::string_view text)
{
    DeviceSelector out;
    const std::string_view s = trim(text);
    if (s.empty()) {
        out.ok = true;
        out.is_default = true;
        out.kind = SelectorKind::Name;
        return out;
    }
    constexpr std::string_view kPrefix = "aaudio:";
    if (s.size() >= kPrefix.size() && s.substr(0, kPrefix.size()) == kPrefix) {
        int id = -1;
        if (!parse_i32(s.substr(kPrefix.size()), id)) {
            out.error = "aaudio id must be a non-negative integer";
            return out;
        }
        out.ok = true;
        out.kind = SelectorKind::AaudioId;
        out.aaudio_id = id;
        return out;
    }
    out.ok = true;
    out.kind = SelectorKind::Name;
    out.name.assign(s.begin(), s.end());
    return out;
}

RouteMatch match_descriptor(const DeviceDescriptor& want,
                            const RouteCandidate* devices,
                            std::size_t count)
{
    RouteMatch miss;
    miss.reason = "device not in the current route list";
    if (want.address.empty() && want.product.empty() && want.type.empty()) {
        miss.reason = "empty device descriptor";
        return miss;
    }
    if (!devices && count > 0) {
        miss.reason = "no route list";
        return miss;
    }

    const bool by_address = !want.address.empty();
    int found = -1;
    int matches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const RouteCandidate& d = devices[i];
        bool hit = false;
        if (by_address) {
            hit = d.address == want.address;
        } else {
            if (!want.type.empty() && d.type != want.type)
                continue;
            if (!want.product.empty() && !same_ci(d.product, want.product))
                continue;
            if (want.product.empty() && want.type.empty())
                continue;
            hit = true;
        }
        if (!hit)
            continue;
        ++matches;
        found = d.id;
    }
    if (matches == 1) {
        RouteMatch ok;
        ok.ok = true;
        ok.id = found;
        ok.reason = by_address ? "address match" : "descriptor match";
        return ok;
    }
    if (matches > 1) {
        miss.reason = "ambiguous device";
        return miss;
    }
    return miss;
}

OpenVerdict verify_opened_device(bool specific, int requested_id, int opened_id) noexcept
{
    OpenVerdict out;
    if (opened_id < 0) {
        out.reason = "opened device is unreadable";
        return out;
    }
    if (specific && opened_id != requested_id) {
        out.reason = "opened device differs from the request";
        return out;
    }
    out.transmit = true;
    out.reason = specific ? "opened device matches" : "default device";
    return out;
}

RouteLoss on_route_lost(bool specific) noexcept
{
    RouteLoss out;
    out.action = RouteLossAction::Stop;
    out.reason = specific ? "selected device lost" : "audio route lost";
    return out;
}

CommChoice select_communication_output(int requested_id,
                                       const RouteCandidate* devices,
                                       std::size_t count)
{
    CommChoice out;
    if (!devices || count == 0) {
        out.reason = "no communication devices";
        return out;
    }
    const RouteCandidate* found = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        if (devices[i].id == requested_id) {
            found = &devices[i];
            break;
        }
    }
    if (!found) {
        out.reason = "not in available communication devices";
        return out;
    }
    if (!found->output) {
        out.reason = "setCommunicationDevice takes an output device; Android selects the matching input";
        return out;
    }
    out.ok = true;
    out.output_id = found->id;
    out.reason = "communication output";
    return out;
}

} // namespace pm::audio::session
