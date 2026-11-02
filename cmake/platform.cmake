if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE
        opengl32
        gdi32
        winmm
        setupapi
        ole32
        avrt
        ksuser
        vorbisenc
        vorbis
        FLAC
        opus
        ogg
        mp3lame
        mpg123
        shlwapi
        bcrypt
        secur32
        ws2_32
        msvcrt
        dbghelp
        psapi
    )
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
  endif()
  if(MINGW)
    target_link_libraries(${PROJECT_NAME} PRIVATE stdc++exp)

    target_link_options(${PROJECT_NAME} PRIVATE
          -static
          -static-libgcc
          -static-libstdc++
      )
  endif()
  set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS "-Wl,--allow-multiple-definition"
    )
elseif(IOS)
  include(${CMAKE_CURRENT_LIST_DIR}/ios.cmake)
elseif(APPLE)
  target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework CoreVideo"
        "-framework IOKit"
        "-framework Cocoa"
        "-framework GLUT"
        "-framework OpenGL"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework VideoToolbox"
        "-framework CoreFoundation"
        "-framework CoreMedia"
    )
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
  endif()
elseif(ANDROID)
  target_link_libraries(${PROJECT_NAME} PRIVATE
        log
        android
        EGL
        GLESv3
        z
        aaudio
        OpenSLES
        vorbis
        vorbisfile
        ogg
    )
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_ANDROID)
  target_link_options(${PROJECT_NAME} PRIVATE -Wl,-z,max-page-size=16384)
elseif(EMSCRIPTEN)
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
  target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_WEB)
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
  endif()
  # catch blocks are dead code on wasm unless compiled with -fexceptions too
  target_compile_options(${PROJECT_NAME} PRIVATE -fexceptions)
  target_link_options(${PROJECT_NAME} PRIVATE
    -sUSE_SDL=3
    -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sPTHREAD_POOL_SIZE=32
    -sINITIAL_MEMORY=134217728
    -sASSERTIONS=1
    -sNO_DISABLE_EXCEPTION_CATCHING
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/shader@/shader"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Songs@/Songs"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/Skins/PyTaikoGreen@/Skins/PyTaikoGreen"
    "SHELL:--exclude-file */Videos/*"
  )
elseif(UNIX)
  target_link_libraries(${PROJECT_NAME} PRIVATE
        GL
        m
        pthread
        dl
        rt
        X11
        asound
    )
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
  endif()
  if(EXISTS "/usr/bin/mold")
    target_link_options(${PROJECT_NAME} PRIVATE -fuse-ld=mold)
  endif()
  find_library(JACK_LIB jack)
  if(JACK_LIB)
    target_link_libraries(${PROJECT_NAME} PUBLIC ${JACK_LIB})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_JACK)
  endif()
  find_library(PULSE_LIB pulse)
  if(PULSE_LIB)
    target_link_libraries(${PROJECT_NAME} PUBLIC ${PULSE_LIB})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_PULSE)
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE SDL_MAIN_HANDLED)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(ANDROID OR IOS OR EMSCRIPTEN)
    target_compile_options(${PROJECT_NAME} PRIVATE
            -O0
            -g
            $<$<CXX_COMPILER_ID:GNU>:-fmax-errors=0>
            -fno-omit-frame-pointer
        )
  elseif(NOT WIN32)
    target_compile_options(${PROJECT_NAME} PRIVATE
            -O0
            -g
            -fmax-errors=0
            -fsanitize=address
            $<$<NOT:$<PLATFORM_ID:Darwin>>:-fsanitize=leak>
            -fsanitize=undefined
            -fno-omit-frame-pointer
        )
    target_link_options(${PROJECT_NAME} PRIVATE
            -fsanitize=address
            $<$<NOT:$<PLATFORM_ID:Darwin>>:-fsanitize=leak>
            -fsanitize=undefined
        )
    message(STATUS "AddressSanitizer enabled for Debug build")
  else()
    target_compile_options(${PROJECT_NAME} PRIVATE
            -O0
            -g
            -gdwarf-4
            -fmax-errors=0
            -fno-omit-frame-pointer
        )
  endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Profiling")
  target_compile_options(${PROJECT_NAME} PRIVATE
        -O2
        -g
        -fno-omit-frame-pointer
        -DNDEBUG
    )
  message(STATUS "Profiler build")
else()
  if(WIN32)
    # Z-DON: -O3 and link-time optimization (the other desktop targets already used -flto).
    # -DZDON_LTO=OFF skips LTO for quick development rebuilds (the LTO link takes minutes).
    option(ZDON_LTO "Link-time optimization for the Windows Release build" ON)
    target_compile_options(${PROJECT_NAME} PRIVATE
          -O3
          -march=x86-64
          -DNDEBUG
      )
    if(ZDON_LTO)
      target_compile_options(${PROJECT_NAME} PRIVATE -flto=auto)
      target_link_options(${PROJECT_NAME} PRIVATE -flto=auto -O3)
    endif()
    # Z-DON: profile-guided optimization, game target only (tools/build_pgo.sh runs the whole cycle).
    # GEN = instrumented exe that writes .gcda files to ZDON_PGO_DIR on exit; USE = optimize with them.
    set(ZDON_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GEN or USE")
    set_property(CACHE ZDON_PGO PROPERTY STRINGS OFF GEN USE)
    set(ZDON_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Profile data directory for ZDON_PGO")
    if(ZDON_PGO STREQUAL "GEN")
      # atomic counters: the game updates them from several threads
      set(_zdon_pgo -fprofile-generate=${ZDON_PGO_DIR} -fprofile-update=atomic)
    elseif(ZDON_PGO STREQUAL "USE")
      # partial training: code the training run never reached stays optimized as without a profile;
      # a profile older than the source only warns (rerun tools/build_pgo.sh)
      set(_zdon_pgo -fprofile-use=${ZDON_PGO_DIR} -fprofile-partial-training -Wno-missing-profile
          -Wno-error=coverage-mismatch)
    elseif(NOT ZDON_PGO STREQUAL "OFF")
      message(FATAL_ERROR "ZDON_PGO must be OFF, GEN or USE")
    endif()
    if(DEFINED _zdon_pgo)
      target_compile_options(${PROJECT_NAME} PRIVATE ${_zdon_pgo})
      target_link_options(${PROJECT_NAME} PRIVATE ${_zdon_pgo})
      message(STATUS "Z-DON PGO: ${ZDON_PGO} (${ZDON_PGO_DIR})")
    endif()
  elseif(IOS)
    target_compile_options(${PROJECT_NAME} PRIVATE -O2 -DNDEBUG -flto=thin)
    target_link_options(${PROJECT_NAME} PRIVATE -flto=thin)
  elseif(APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE
          -O2
          -DNDEBUG
          -flto=auto
      )
  elseif(ANDROID)
    target_compile_options(${PROJECT_NAME} PRIVATE
          -O2
          -DNDEBUG
          -flto=thin
      )
    target_link_options(${PROJECT_NAME} PRIVATE -flto=thin)
  elseif(EMSCRIPTEN)
    target_compile_options(${PROJECT_NAME} PRIVATE
          -O2
          -DNDEBUG
      )
  else()
    target_compile_options(${PROJECT_NAME} PRIVATE
          -O2
          -march=x86-64
          -DNDEBUG
          -flto=auto
      )
    target_link_options(${PROJECT_NAME} PRIVATE -flto=auto)
  endif()
endif()

if(YATAIDON_USE_SDL2)
  target_compile_definitions(${PROJECT_NAME} PRIVATE YATAIDON_USE_SDL2=1)
  target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
endif()
