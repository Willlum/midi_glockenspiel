set(src_dirs                #"./"
                            "./app")

set(include_dirs            #"./"
                            "./app"
                            "./include")

set(requires fatfs esp_timer)

idf_component_register(SRC_DIRS "${src_dirs}"
                       INCLUDE_DIRS "${include_dirs}"
                       REQUIRES "${requires}"
                       )