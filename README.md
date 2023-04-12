# Prismatic

This is my version of [Prismatic](https://github.com/prism-em/prismatic), a (scanning) transmission electron microscopy simulation software using the PRISM algorithm and the multislice method.

The [Prismatic website](http://www.prism-em.com) has documentation and compilation instructions.

## Changes
* Can add an additional phase shift into the transmission function calculated for each slice.  Control using the options `--import-extra-potential`, `--extra-potential-fac`, `--extra-potential-type`, `--import-file`, `--import-data-path`.
* Fixed a bug related to cropping CBED patterns in 4D-STEM simulations.
* Other minor changes.

## Other notes
I have compiled Prismatic on Red Hat Enterprise Linux 7.9 with gcc 7.2.0, CUDA 10.0, HDF5 1.12, and FFTW 3.3.10.