rm -f ./build ./build.cpp.o.meta

g++ $LDFLAGS $CPPFLAGS -g --std=gnu++23 -I ../../ -o ./build build.cpp && ./build
