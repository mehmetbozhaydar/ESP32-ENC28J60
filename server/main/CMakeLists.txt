set(srcs)

idf_component_register(SRCS "enc28j60_server.c" "${srcs}"
                       INCLUDE_DIRS ".")
