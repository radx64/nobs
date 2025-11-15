rm ./build ./build.cpp.meta

g++ $LDFLAGS $CPPFLAGS -g --std=gnu++23 -I ../../ -o ./build build.cpp && ./build
