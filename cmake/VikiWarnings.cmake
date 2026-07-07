# Warning flags applied to VikiCAD's own targets (not vendored code).
add_library(viki_warnings INTERFACE)
target_compile_options(viki_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wcast-align -Wdouble-promotion
)
