set(res "${CMAKE_CURRENT_LIST_DIR}/../../../thorvg/test/res")
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp-thorvg esp-quickjs-ng esp_timer mbedtls
                       EMBED_TXTFILES "main.js"
                       EMBED_FILES "${res}/test0.svg" "${res}/test.lot" "${res}/segment.lot"
                                   "${res}/slot.lot" "${res}/PublicSans-Regular.ttf")
