FILE(GLOB SRC *.cpp *.h *.rc)

SOURCE_GROUP("" FILES ${SRC})

ADD_EXECUTABLE(tga2dds ${SRC})

TARGET_LINK_LIBRARIES(tga2dds s3tc_compressor nelmisc nel3d)
NL_DEFAULT_PROPS(tga2dds "NeL, Tools, 3D: tga2dds")
NL_ADD_RUNTIME_FLAGS(tga2dds)

INSTALL(TARGETS tga2dds RUNTIME DESTINATION ${NL_BIN_PREFIX} COMPONENT tools3d)
