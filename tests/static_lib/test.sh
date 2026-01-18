set -e
echo "Building nobs"
rm -f ./build ./build.cpp.o.meta
g++ -g -std=gnu++23 -I ../../ -o ./build build.cpp 
echo "Running build"
./build
echo "Running built application"
./build_dir/demo
