#include "socket_support.h"
#include <tinyxml2.h>
#include <curl/curl.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <memory>
#include <sstream>

namespace torrent::mapping {
namespace {
using tinyxml2::XMLElement;
const char* name(const XMLElement* element) {
    const auto full = element->Name(); const auto colon = std::strrchr(full, ':'); return colon ? colon + 1 : full;
}
XMLElement* find(XMLElement* element, const std::string& wanted) {
    if (!element) return nullptr;
    if (name(element) == wanted) return element;
    for (auto child = element->FirstChildElement(); child; child = child->NextSiblingElement())
        if (auto result = find(child, wanted)) return result;
    return nullptr;
}
std::string text(XMLElement* element, const std::string& wanted) {
    const auto child = find(element, wanted);
    return child && child->GetText() ? child->GetText() : "";
}
void parse(tinyxml2::XMLDocument& document, const std::string& data) {
    if (document.Parse(data.data(), data.size()) != tinyxml2::XML_SUCCESS)
        throw std::runtime_error("Invalid UPnP XML response");
}
uint32_t number(const std::string& text) {
    const auto first = text.find_first_not_of(" \r\n\t"), last = text.find_last_not_of(" \r\n\t");
    uint32_t value = 0;
    if (first == std::string::npos) throw std::runtime_error("Missing UPnP numeric value");
    const auto result = std::from_chars(text.data() + first, text.data() + last + 1, value);
    if (result.ec != std::errc{} || result.ptr != text.data() + last + 1) throw std::runtime_error("Invalid UPnP numeric value");
    return value;
}
struct Fault : std::runtime_error {
    uint32_t code;
    explicit Fault(uint32_t value) : std::runtime_error("UPnP error " + std::to_string(value)), code(value) {}
};
std::string wireProtocol(PortTransport transport) { return transport == PortTransport::Tcp ? "TCP" : "UDP"; }
size_t receive(void* bytes, size_t size, size_t count, void* destination) {
    constexpr size_t maximum = 256 * 1024;
    auto& output = *static_cast<std::string*>(destination);
    if (size && count > maximum / size) return 0;
    const auto length = size * count;
    if (length > maximum - output.size()) return 0;
    output.append(static_cast<char*>(bytes), length); return length;
}
int cancel(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& cancelled = *static_cast<const Cancelled*>(context); return cancelled && cancelled() ? 1 : 0;
}
}

std::string GatewayClient::sameGatewayUrl(const std::string& base, const std::string& relative) const {
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, base.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK ||
        (!relative.empty() && curl_url_set(url.get(), CURLUPART_URL, relative.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK))
        throw std::runtime_error("Invalid UPnP device URL");
    const auto part = [&](CURLUPart field) {
        char* value = nullptr;
        if (curl_url_get(url.get(), field, &value, 0) != CURLUE_OK) throw std::runtime_error("Invalid UPnP device URL");
        std::string result(value); curl_free(value); return result;
    };
    if (part(CURLUPART_SCHEME) != "http" || part(CURLUPART_HOST) != gateway_)
        throw std::runtime_error("UPnP control URL must belong to the selected IPv4 gateway");
    return part(CURLUPART_URL);
}

std::string GatewayClient::http(const std::string& url, const std::string& action, const std::string& body, const Cancelled& cancelled) {
    checkCancelled(cancelled);
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Cannot initialize UPnP HTTP request");
    std::string result;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "HTTP");
#else
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP));
#endif
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(options_.request_timeout_ms));
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options_.request_timeout_ms));
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, cancel);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &cancelled);
    curl_slist* headers = nullptr;
    if (!action.empty()) {
        headers = curl_slist_append(headers, "Content-Type: text/xml; charset=utf-8");
        headers = curl_slist_append(headers, ("SOAPAction: \"" + service_ + "#" + action + "\"").c_str());
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned_headers(headers, curl_slist_free_all);
    const auto error = curl_easy_perform(handle.get());
    if (error != CURLE_OK) throw std::runtime_error(std::string("UPnP HTTP: ") + curl_easy_strerror(error));
    long code = 0; curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &code);
    if (code != 200 && !(code == 500 && !action.empty())) throw std::runtime_error("UPnP HTTP status " + std::to_string(code));
    return result;
}

std::string GatewayClient::soap(const std::string& action, const std::vector<std::pair<std::string, std::string>>& arguments,
                               const Cancelled& cancelled) {
    tinyxml2::XMLDocument request;
    auto envelope = request.NewElement("s:Envelope");
    envelope->SetAttribute("xmlns:s", "http://schemas.xmlsoap.org/soap/envelope/");
    envelope->SetAttribute("s:encodingStyle", "http://schemas.xmlsoap.org/soap/encoding/");
    request.InsertEndChild(envelope);
    auto body = request.NewElement("s:Body"); envelope->InsertEndChild(body);
    auto command = request.NewElement(("u:" + action).c_str()); command->SetAttribute("xmlns:u", service_.c_str());
    body->InsertEndChild(command);
    for (const auto& argument : arguments) {
        auto field = request.NewElement(argument.first.c_str()); field->SetText(argument.second.c_str()); command->InsertEndChild(field);
    }
    tinyxml2::XMLPrinter printer; request.Print(&printer);
    const auto response = http(control_url_, action, printer.CStr(), cancelled);
    tinyxml2::XMLDocument document; parse(document, response);
    if (auto fault = find(document.RootElement(), "Fault")) throw Fault(number(text(fault, "errorCode")));
    if (!find(document.RootElement(), action + "Response")) throw std::runtime_error("Missing UPnP action response");
    return response;
}

std::string GatewayClient::discoverUpnp(const Cancelled& cancelled) {
    Socket socket;
    const auto local = endpoint(local_address_, 0), gateway = endpoint(gateway_, 0);
    if (::bind(socket.value, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
        throw std::runtime_error("Cannot bind SSDP discovery to the router interface");
    const auto remote = endpoint(options_.discovery_address, options_.discovery_port);
    const int ttl = 2;
    setsockopt(socket.value, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    setsockopt(socket.value, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local.sin_addr), sizeof(local.sin_addr));
    const std::string request = "M-SEARCH * HTTP/1.1\r\nHOST: " + options_.discovery_address + ":" + std::to_string(options_.discovery_port) +
        "\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.discovery_timeout_ms);
    const auto interrupted = [&] { return (cancelled && cancelled()) || std::chrono::steady_clock::now() >= end; };
    auto next = std::chrono::steady_clock::time_point{};
    while (!interrupted()) {
        if (std::chrono::steady_clock::now() >= next) {
            sendto(socket.value, request.data(), static_cast<int>(request.size()), 0,
                reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        if (!readable(socket)) continue;
        std::array<char, 8192> bytes{}; sockaddr_in source{}; socklen_t size = sizeof(source);
        const auto received = recvfrom(socket.value, bytes.data(), static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&source), &size);
        if (received <= 0 || source.sin_addr.s_addr != gateway.sin_addr.s_addr) continue;
        std::istringstream response(std::string(bytes.data(), received));
        std::string line, location; std::getline(response, line);
        if (line.find(" 200 ") == std::string::npos) continue;
        while (std::getline(response, line)) {
            const auto colon = line.find(':'); if (colon == std::string::npos) continue;
            auto header = line.substr(0, colon);
            std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (header != "location") continue;
            const auto first = line.find_first_not_of(" \t", colon + 1), last = line.find_last_not_of(" \r\t");
            if (first != std::string::npos) location = line.substr(first, last - first + 1);
        }
        if (location.empty()) continue;
        try { location = sameGatewayUrl(location); } catch (const std::exception&) { continue; }
        tinyxml2::XMLDocument device; parse(device, http(location, "", "", interrupted));
        XMLElement* service = nullptr;
        const auto choose = [&](const auto& self, XMLElement* element) -> void {
            if (!element || service) return;
            if (std::string(name(element)) == "service") {
                const auto type = text(element, "serviceType");
                if (type == "urn:schemas-upnp-org:service:WANIPConnection:1" || type == "urn:schemas-upnp-org:service:WANIPConnection:2" ||
                    type == "urn:schemas-upnp-org:service:WANPPPConnection:1") { service = element; return; }
            }
            for (auto child = element->FirstChildElement(); child; child = child->NextSiblingElement()) self(self, child);
        };
        choose(choose, device.RootElement());
        if (!service) throw std::runtime_error("No supported UPnP Internet Gateway service");
        service_ = text(service, "serviceType");
        const auto base = text(device.RootElement(), "URLBase");
        const auto control = text(service, "controlURL");
        if (control.empty()) throw std::runtime_error("Missing UPnP control URL");
        control_url_ = sameGatewayUrl(base.empty() ? location : sameGatewayUrl(base), control);
        tinyxml2::XMLDocument external; parse(external, soap("GetExternalIPAddress", {}, interrupted));
        external_address_ = text(external.RootElement(), "NewExternalIPAddress");
        if (endpoint(external_address_, 1).sin_addr.s_addr == 0) throw std::runtime_error("UPnP returned an empty external address");
        return control_url_;
    }
    checkCancelled(cancelled);
    throw std::runtime_error("UPnP discovery timed out");
}

Lease GatewayClient::map(PortTransport transport, uint16_t internal, uint16_t external,
                         const std::string& owner, bool& attempted, const Cancelled& cancelled) {
    if (protocol_ == MappingProtocol::NatPmp) { attempted = true; return natpmp(transport, internal, external, options_.lease_seconds, cancelled); }
    const auto arguments = std::vector<std::pair<std::string, std::string>>{
        {"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(external)}, {"NewProtocol", wireProtocol(transport)}};
    try {
        tinyxml2::XMLDocument existing; parse(existing, soap("GetSpecificPortMappingEntry", arguments, cancelled));
        if (text(existing.RootElement(), "NewInternalClient") != local_address_ ||
            number(text(existing.RootElement(), "NewInternalPort")) != internal || text(existing.RootElement(), "NewPortMappingDescription") != owner)
            throw std::runtime_error("UPnP port already belongs to another mapping");
    } catch (const Fault& fault) { if (fault.code != 714) throw; }
    auto create = arguments;
    create.insert(create.end(), {{"NewInternalPort", std::to_string(internal)}, {"NewInternalClient", local_address_},
        {"NewEnabled", "1"}, {"NewPortMappingDescription", owner}, {"NewLeaseDuration", std::to_string(options_.lease_seconds)}});
    attempted = true;
    soap("AddPortMapping", create, cancelled);
    tinyxml2::XMLDocument check; parse(check, soap("GetSpecificPortMappingEntry", arguments, cancelled));
    if (text(check.RootElement(), "NewInternalClient") != local_address_ || number(text(check.RootElement(), "NewInternalPort")) != internal ||
        text(check.RootElement(), "NewPortMappingDescription") != owner ||
        (text(check.RootElement(), "NewEnabled") != "1" && text(check.RootElement(), "NewEnabled") != "true"))
        throw std::runtime_error("UPnP mapping ownership could not be verified");
    const auto duration = number(text(check.RootElement(), "NewLeaseDuration"));
    if (!duration) {
        remove(transport, internal, external, owner, cancelled);
        throw std::runtime_error("UPnP router did not provide a finite lease");
    }
    return {external, duration};
}

void GatewayClient::remove(PortTransport transport, uint16_t internal, uint16_t external,
                           const std::string& owner, const Cancelled& cancelled) {
    if (protocol_ == MappingProtocol::NatPmp) { natpmp(transport, internal, 0, 0, cancelled); return; }
    const auto arguments = std::vector<std::pair<std::string, std::string>>{
        {"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(external)}, {"NewProtocol", wireProtocol(transport)}};
    try {
        tinyxml2::XMLDocument existing; parse(existing, soap("GetSpecificPortMappingEntry", arguments, cancelled));
        if (text(existing.RootElement(), "NewInternalClient") != local_address_ ||
            number(text(existing.RootElement(), "NewInternalPort")) != internal || text(existing.RootElement(), "NewPortMappingDescription") != owner)
            throw std::runtime_error("UPnP mapping owner changed; removal skipped");
        soap("DeletePortMapping", arguments, cancelled);
    } catch (const Fault& fault) { if (fault.code != 714) throw; }
}

} // namespace torrent::mapping
