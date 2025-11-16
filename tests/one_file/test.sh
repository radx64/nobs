set -e
echo "Building nobs"
g++ -g --std=gnu++23 -I ../../ -o ./build build.cpp
echo "Running build"
./build
echo "Running built application"
./build_dir/one_file_app

