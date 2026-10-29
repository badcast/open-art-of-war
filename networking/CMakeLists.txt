# OpenAOWNetworking: the network layer of the game as a static library. Its public headers
# (include/openaow/net) show no ENet and no platform sockets; it knows nothing about the game itself,
# so the dedicated server links it without SDL or any game code.

# --- ENet 1.3.18 (3rdParty/enet, MIT): reliable / unreliable channels over one UDP socket -----------
set(ENET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdParty/enet)
add_library(oaow-enet STATIC
    ${ENET_DIR}/callbacks.c
    ${ENET_DIR}/compress.c
    ${ENET_DIR}/host.c
    ${ENET_DIR}/list.c
    ${ENET_DIR}/packet.c
    ${ENET_DIR}/peer.c
    ${ENET_DIR}/protocol.c
    ${ENET_DIR}/unix.c
    ${ENET_DIR}/win32.c
)
target_include_directories(oaow-enet PUBLIC ${ENET_DIR}/include)
set_target_properties(oaow-enet PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
# Third-party code: built without the project's warnings
if(MSVC)
    target_compile_options(oaow-enet PRIVATE /w)
else()
    target_compile_options(oaow-enet PRIVATE -w)
endif()
if(WIN32)
    target_link_libraries(oaow-enet PUBLIC ws2_32 winmm)
else()
    include(CheckFunctionExists)
    include(CheckStructHasMember)
    include(CheckTypeSize)
    check_function_exists("fcntl" OAOW_ENET_HAS_FCNTL)
    check_function_exists("poll" OAOW_ENET_HAS_POLL)
    check_function_exists("getaddrinfo" OAOW_ENET_HAS_GETADDRINFO)
    check_function_exists("getnameinfo" OAOW_ENET_HAS_GETNAMEINFO)
    check_function_exists("inet_pton" OAOW_ENET_HAS_INET_PTON)
    check_function_exists("inet_ntop" OAOW_ENET_HAS_INET_NTOP)
    check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" OAOW_ENET_HAS_MSGHDR_FLAGS)
    set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
    check_type_size("socklen_t" OAOW_ENET_HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
    unset(CMAKE_EXTRA_INCLUDE_FILES)
    foreach(feature FCNTL POLL GETADDRINFO GETNAMEINFO INET_PTON INET_NTOP MSGHDR_FLAGS SOCKLEN_T)
        if(OAOW_ENET_HAS_${feature})
            target_compile_definitions(oaow-enet PRIVATE HAS_${feature}=1)
        endif()
    endforeach()
endif()

# --- OpenAOWNetworking ----------------------------------------------------------------------------
add_library(OpenAOWNetworking STATIC
    src/protocol/Crc32.cpp
    src/protocol/Protocol.cpp
    src/transport/EnetSupport.cpp
    src/transport/EnetTransport.cpp
    src/transport/LoopbackTransport.cpp
    src/session/SessionHost.cpp
    src/session/SessionClient.cpp
    src/discovery/Discovery.cpp
    src/server/RoomServer.cpp
    src/server/StatusPage.cpp
    src/server/MasterClient.cpp
    src/server/Rendezvous.cpp
)
target_include_directories(OpenAOWNetworking
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Linked into the shared engine library as well
set_target_properties(OpenAOWNetworking PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(OpenAOWNetworking PUBLIC cxx_std_17)
# ENet stays an implementation detail: callers never see its headers
target_link_libraries(OpenAOWNetworking PRIVATE oaow-enet)
if(WIN32)
    target_link_libraries(OpenAOWNetworking INTERFACE ws2_32 winmm)
endif()

# --- Tests ----------------------------------------------------------------------------------------
option(OAOW_BUILD_NET_TESTS "Build the OpenAOWNetworking unit tests" ON)
if(OAOW_BUILD_NET_TESTS)
    add_executable(openaow-net-tests tests/NetTests.cpp)
    # The tests also reach into the internals (ENet setup)
    target_include_directories(openaow-net-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(openaow-net-tests PRIVATE OpenAOWNetworking oaow-enet)
    set_target_properties(openaow-net-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME openaow-net-tests COMMAND openaow-net-tests)
endif()
