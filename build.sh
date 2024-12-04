#!/bin/bash

export OS=Linux.conda
export CPU=x86_64

export OMPI_CC=mpicc
export OMPI_CXX=mpic++
export OMPI_FC=mpifort

cd src/rh
make clean
make

cd rh_1d
make clean
make

cd ../..
make clean
make