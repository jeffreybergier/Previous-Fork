
if(Readline_FOUND)
	include_directories(${READLINE_INCLUDE_DIR})
endif(Readline_FOUND)

if(ENABLE_DSP_EMU)
	set(DSPDBG_C debugdsp.c)
endif(ENABLE_DSP_EMU)

include_directories(${SDL2_INCLUDE_DIRS})

add_library(Debug
	    log.c debugui.c breakcond.c debugcpu.c debugInfo.c
	    ${DSPDBG_C} evaluate.c history.c symbols.c vars.c
	    profile.c profilecpu.c profiledsp.c 68kDisass.c)

target_link_libraries(Debug PRIVATE ${SDL2_LIBRARIES})
