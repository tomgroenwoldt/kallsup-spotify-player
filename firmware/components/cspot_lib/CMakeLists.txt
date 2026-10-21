# Builds cspot + the parts of bell it needs from the unmodified submodule in
# ../cspot, as one ESP-IDF component. See docs/CSPOT.md.
#
# Upstream's own CMakeLists aren't used: they run the nanopb generator at build
# time (needs protoc and an old Python protobuf) and pull in codecs, sinks, a
# web server and an mDNS stack this firmware doesn't need.

set(CSPOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../cspot/cspot")
set(BELL_DIR "${CSPOT_DIR}/bell")

if(NOT EXISTS "${CSPOT_DIR}/src/SpircHandler.cpp")
    message(FATAL_ERROR "cspot submodule missing: run `git submodule update --init --recursive`")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/overlays.cmake")

# The checked-in nanopb output must match the submodule's .proto/.options.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/generated/SOURCES.sha256" proto_sums)
foreach(line IN LISTS proto_sums)
    string(REGEX MATCH "^([0-9a-f]+)  (.+)$" _ "${line}")
    file(SHA256 "${CSPOT_DIR}/${CMAKE_MATCH_2}" actual)
    if(NOT actual STREQUAL CMAKE_MATCH_1)
        message(FATAL_ERROR "${CMAKE_MATCH_2} changed in the cspot submodule; "
                            "regenerate generated/protobuf (docs/CSPOT.md)")
    endif()
endforeach()

# --- cspot ---
file(GLOB cspot_srcs "${CSPOT_DIR}/src/*.cpp")
file(GLOB proto_srcs "${CMAKE_CURRENT_LIST_DIR}/generated/protobuf/*.c")

# --- bell, built like squeezelite-esp32 does: no codecs, sinks, MQTT, web
# server, fmt or regex; cJSON from ESP-IDF; Vorbis via tremor ---
file(GLOB bell_srcs
    "${BELL_DIR}/external/nanopb/*.c"
    "${BELL_DIR}/main/utilities/*.cpp" "${BELL_DIR}/main/utilities/*.c"
    "${BELL_DIR}/main/io/*.cpp" "${BELL_DIR}/main/io/*.c"
    "${BELL_DIR}/main/platform/esp/WrappedSemaphore.cpp"
    "${BELL_DIR}/external/tremor/*.c"
)
list(REMOVE_ITEM bell_srcs
    "${BELL_DIR}/main/io/BellMQTTClient.cpp"
    "${BELL_DIR}/main/io/BellHTTPServer.cpp"
    "${BELL_DIR}/main/io/MGStreamAdapter.cpp"
    "${BELL_DIR}/main/io/EncodedAudioStream.cpp"
    "${BELL_DIR}/external/tremor/ivorbisfile_example.c"
)

# --- source overlays (fixes applied to copies; the submodule stays clean) ---

# 1. Build fix: strstr() without <cstring> fails under GCC 14.
cspot_overlay("${BELL_DIR}/main/io/URLParser.cpp" bell_srcs
    "#include \"URLParser.h\"\n"
    "#include \"URLParser.h\"\n\n#include <cstring>\n"
)

# 2. Crash fix: apresolve answer used unchecked; anything but the expected JSON
#    (captive portal, error page) dereferenced NULL -> reboot loop.
cspot_overlay("${CSPOT_DIR}/src/ApResolve.cpp" cspot_srcs
    "#include <vector>            // for vector\n"
    "#include <vector>            // for vector\n#include <stdexcept>\n"
    "  auto ap_string = std::string(\n      cJSON_GetArrayItem(cJSON_GetObjectItem(json, \"ap_list\"), 0)->valuestring);\n"
    "  cJSON* ap_list = json ? cJSON_GetObjectItem(json, \"ap_list\") : nullptr;\n  cJSON* first = cJSON_IsArray(ap_list) ? cJSON_GetArrayItem(ap_list, 0) : nullptr;\n  if (!cJSON_IsString(first)) {\n    cJSON_Delete(json);\n    throw std::runtime_error(\"apresolve: unexpected response\");\n  }\n  auto ap_string = std::string(first->valuestring);\n"
)

# 3. Robustness: the dispatcher task runs reconnects incl. a TLS handshake;
#    4 kB of stack is too little for that. Stack lives in PSRAM.
# 7. Crash fix: the callback tables were filled under connectingMutex but read,
#    called and erased with no lock from handlePacket(); quick track skips
#    corrupted the unordered_map (LoadProhibited in handlePacket). One mutex now
#    guards all three tables; callbacks are copied out and run unlocked.
cspot_overlay("${CSPOT_DIR}/src/MercurySession.cpp" cspot_srcs
    "bell::Task(\"mercury_dispatcher\", 4 * 1024, 3, 1)"
    "bell::Task(\"mercury_dispatcher\", 16 * 1024, 3, 1)"
    "#include <utility>      // for pair\n"
    "#include <utility>      // for pair\n\n// Guards callbacks, audioKeyCallbacks and subscriptions (overlay 7).\nnamespace {\nstd::mutex s_callbacksMutex;\n}\n"
    "void MercurySession::unregister(uint64_t sequenceId) {\n  auto callback"
    "void MercurySession::unregister(uint64_t sequenceId) {\n  std::scoped_lock cbLock(s_callbacksMutex);\n  auto callback"
    "void MercurySession::unregisterAudioKey(uint32_t sequenceId) {\n  auto callback"
    "void MercurySession::unregisterAudioKey(uint32_t sequenceId) {\n  std::scoped_lock cbLock(s_callbacksMutex);\n  auto callback"
    "      if (this->audioKeyCallbacks.count(seqId) > 0) {\n        auto success = static_cast<RequestType>(packet.command) ==\n                       RequestType::AUDIO_KEY_SUCCESS_RESPONSE;\n        this->audioKeyCallbacks[seqId](success, packet.data);\n      }"
    "      AudioKeyCallback audioCallback;\n      {\n        std::scoped_lock cbLock(s_callbacksMutex);\n        auto it = this->audioKeyCallbacks.find(seqId);\n        if (it != this->audioKeyCallbacks.end()) audioCallback = it->second;\n      }\n      if (audioCallback) {\n        auto success = static_cast<RequestType>(packet.command) ==\n                       RequestType::AUDIO_KEY_SUCCESS_RESPONSE;\n        audioCallback(success, packet.data);\n      }"
    "      if (this->callbacks.count(response.sequenceId) > 0) {\n        auto seqId = response.sequenceId;\n        this->callbacks[response.sequenceId](response);\n        this->callbacks.erase(this->callbacks.find(seqId));\n      }"
    "      ResponseCallback callback;\n      {\n        std::scoped_lock cbLock(s_callbacksMutex);\n        auto it = this->callbacks.find(response.sequenceId);\n        if (it != this->callbacks.end()) {\n          callback = std::move(it->second);\n          this->callbacks.erase(it);\n        }\n      }\n      if (callback) callback(response);"
    "      if (this->subscriptions.count(uri) > 0) {\n        this->subscriptions[uri](response);\n      }"
    "      ResponseCallback subscription;\n      {\n        std::scoped_lock cbLock(s_callbacksMutex);\n        auto it = this->subscriptions.find(uri);\n        if (it != this->subscriptions.end()) subscription = it->second;\n      }\n      if (subscription) subscription(response);"
    "  // Fail all callbacks\n  for (auto& it : this->callbacks) {\n    it.second(response);\n  }\n\n  // Fail all subscriptions\n  for (auto& it : this->subscriptions) {\n    it.second(response);\n  }\n\n  // Remove references\n  this->subscriptions = {};\n  this->callbacks = {};"
    "  decltype(this->callbacks) pending;\n  decltype(this->subscriptions) subs;\n  {\n    std::scoped_lock cbLock(s_callbacksMutex);\n    pending.swap(this->callbacks);\n    subs.swap(this->subscriptions);\n  }\n  for (auto& it : pending) it.second(response);\n  for (auto& it : subs) it.second(response);"
    "    this->subscriptions.insert({uri, subscription});"
    "    std::scoped_lock cbLock(s_callbacksMutex);\n    this->subscriptions.insert({uri, subscription});"
    "  this->callbacks.insert({sequenceId, callback});"
    "  {\n    std::scoped_lock cbLock(s_callbacksMutex);\n    this->callbacks.insert({sequenceId, callback});\n  }"
    "  this->audioKeyCallbacks.insert({this->audioKeySequence, audioCallback});"
    "  {\n    std::scoped_lock cbLock(s_callbacksMutex);\n    this->audioKeyCallbacks.insert({this->audioKeySequence, audioCallback});\n  }"
)

# 4. + 5. TLSSocket: verify server certificates against ESP-IDF's CA bundle
#    (upstream verifies nothing unless a bundle file is loaded), and give
#    sockets 15 s timeouts so a dead connection fails instead of hanging.
cspot_overlay("${BELL_DIR}/main/io/TLSSocket.cpp" bell_srcs
    "#include <stdexcept>              // for runtime_error\n"
    "#include <stdexcept>              // for runtime_error\n#include <sys/socket.h>\n#include \"esp_crt_bundle.h\"\n"
    "    BELL_LOG(error, \"http_tls\", \"failed! connect returned %d\\n\", ret);\n  }\n"
    "    BELL_LOG(error, \"http_tls\", \"failed! connect returned %d\\n\", ret);\n  } else {\n    struct timeval tv = {15, 0};\n    setsockopt(server_fd.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));\n    setsockopt(server_fd.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));\n  }\n"
    "  if (bell::X509Bundle::shouldVerify()) {\n    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);\n  } else {\n    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);\n  }\n"
    "  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);\n  if ((ret = esp_crt_bundle_attach(&conf)) != 0) {\n    throw std::runtime_error(\"esp_crt_bundle_attach failed\");\n  }\n"
)

# 6. Crash fix: the access-token request (client credentials) could throw on a
#    network error with nothing catching it, and its JSON was used unchecked.
cspot_overlay("${CSPOT_DIR}/src/AccessKeyFetcher.cpp" cspot_srcs
    "    auto response = bell::HTTPClient::post(\n        \"https://accounts.spotify.com/api/token\",\n        {{\"Content-Type\", \"application/x-www-form-urlencoded\"}}, body);\n\n#ifdef BELL_ONLY_CJSON\n    cJSON* root = cJSON_ParseWithLength(response->body().data(), response->body().size());\n    if (!cJSON_GetObjectItem(root, \"error\")) {\n      accessKey =\n          std::string(cJSON_GetObjectItem(root, \"access_token\")->valuestring);\n      int expiresIn = cJSON_GetObjectItem(root, \"expires_in\")->valueint;\n"
    "#ifdef BELL_ONLY_CJSON\n    cJSON* root = nullptr;\n    try {\n      auto response = bell::HTTPClient::post(\n          \"https://accounts.spotify.com/api/token\",\n          {{\"Content-Type\", \"application/x-www-form-urlencoded\"}}, body);\n      root = cJSON_ParseWithLength(response->body().data(), response->body().size());\n    } catch (const std::exception& e) {\n      CSPOT_LOG(error, \"Access token request failed: %s\", e.what());\n    }\n    cJSON* token = root ? cJSON_GetObjectItem(root, \"access_token\") : nullptr;\n    cJSON* expires = root ? cJSON_GetObjectItem(root, \"expires_in\") : nullptr;\n    if (cJSON_IsString(token) && cJSON_IsNumber(expires)) {\n      accessKey = std::string(token->valuestring);\n      int expiresIn = expires->valueint;\n"
    "      CSPOT_LOG(error, \"Failed to fetch access token\");\n"
    "      cJSON_Delete(root);\n      CSPOT_LOG(error, \"Failed to fetch access token\");\n"
)

# 8. Crash fix: a Load frame without a context (what the Web API sends for
#    "play these uris", i.e. one track) has context_uri == NULL, which
#    syncWithRemote() passed to strlen()/strcpy(). Treated as "" now.
cspot_overlay("${CSPOT_DIR}/src/PlaybackState.cpp" cspot_srcs
    "  innerFrame.state.context_uri = (char*)realloc(\n      innerFrame.state.context_uri, strlen(remoteFrame.state.context_uri) + 1);\n\n  strcpy(innerFrame.state.context_uri, remoteFrame.state.context_uri);"
    "  const char* remoteContext =\n      remoteFrame.state.context_uri ? remoteFrame.state.context_uri : \"\";\n  innerFrame.state.context_uri = (char*)realloc(\n      innerFrame.state.context_uri, strlen(remoteContext) + 1);\n\n  strcpy(innerFrame.state.context_uri, remoteContext);"
)

idf_component_register(
    SRCS ${cspot_srcs} ${proto_srcs} ${bell_srcs}
    INCLUDE_DIRS
        "${CSPOT_DIR}/include"
        "generated"
        "${BELL_DIR}/main/io/include"
        "${BELL_DIR}/main/utilities/include"
        "${BELL_DIR}/main/platform"
        "${BELL_DIR}/external/nanopb"
        "${BELL_DIR}/external/tremor"
    REQUIRES mbedtls json lwip pthread esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    PB_ENABLE_MALLOC PB_FIELD_32BIT
    BELL_DISABLE_CODECS BELL_DISABLE_FMT BELL_DISABLE_REGEX BELL_ONLY_CJSON
    USE_DEFAULT_STDLIB=1 TARGET_OS_IPHONE=0
)
# Third-party code: don't let its warnings flood or fail the build.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
