# Prismatic

This is my version of [Prismatic](https://github.com/prism-em/prismatic), a (scanning) transmission electron microscopy simulation software using the PRISM algorithm and the multislice method.

The [Prismatic website](http://www.prism-em.com) has documentation and compilation instructions.

## Changes
* Can add an additional phase shift into the transmission function calculated for each slice.  Control using the options `--import-extra-potential`, `--extra-potential-fac`, `--extra-potential-type`, `--import-file`, `--import-data-path`.
* Fixed a bug related to cropping CBED patterns in 4D-STEM simulations.
* Cleaned up how arbitrary aberrations are handled. See below.
* Other minor changes.

## Aberration file input format

Arbitrary geometric aberrations can optionally be specified using a separate input text file.  The file format is given below.  Note that the format has been updated as of 2026-02 and is different from the original version described on the [prismatic website](https://prism-em.com/docs-inputs/).

* 1 comment line
* N lines of the comma separated-form `n`, `m`, `C_mag`, `phi`, where `n` is the radial order of the aberration, `m` is the azimuthal order of the aberration, `C_mag` is the magnitude of the aberration in units of angstroms, and `phi` is the angle of the aberration in degrees.
* -1 to indicate the end of the file.

The phase error $\chi$ is calculated as $\chi(\alpha,\phi) = \frac{2\pi}{\lambda} \sum_{mn} \frac{1}{n+1} C_{nm} \alpha^{n+1} \cos \left[ m (\phi - \phi_{nm}) \right]$, where $C_{nm}$ and $\phi_{nm}$ respectively correspond to the `C_mag` and `phi` values specified in the aberration file. See _Advanced Computing in Electron Microscopy_ by Earl Kirkland for more information.

Note: Probe defocus, C3, and C5 values specified via command line arguments will override any values specified in the aberration file for the same kinds of aberrations.

## Other notes
I have compiled Prismatic on Red Hat Enterprise Linux 7.9 with gcc 7.2.0, CUDA 10.0, HDF5 1.12, and FFTW 3.3.10.