#!/bin/bash

SCRIPT=`realpath $0`
SCRIPTPATH=`dirname $SCRIPT`
TREEBEARD_DIR=`dirname $SCRIPTPATH`
echo "TreeBeard directory : $TREEBEARD_DIR"

# Create the binary directory 
BINARIES_DIR="$TREEBEARD_DIR/runtime_test_binaries"
MKDIR_CMD="mkdir -p $BINARIES_DIR"
echo "$MKDIR_CMD"
$MKDIR_CMD

while getopts "si" opt
do
   case "$opt" in
      s ) SPARSE_FLAG="-s"
          ;;
      i ) INVERT_FLAG="-i"
          ;;

      # ? ) helpFunction ;; # Print helpFunction in case parameter is non-existent
   esac
done

# Build all currently supported models
declare -a modelsArr=("abalone" "airline" "airline-ohe" "bosch" "epsilon" "higgs" "year_prediction_msd")
for i in "${modelsArr[@]}"
do
   BUILD_CMD="./build_xgboost_so.sh -t 8 -b 200 -m $i -o $BINARIES_DIR $SPARSE_FLAG $INVERT_FLAG"
   echo "$BUILD_CMD"
   $BUILD_CMD
done