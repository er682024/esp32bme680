idf_component_register(
    SRCS "bme68x.c" "bme680_wrapper.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver
)

