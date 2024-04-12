package OpenSSL::safe::installdata;

use strict;
use warnings;
use Exporter;
our @ISA = qw(Exporter);
our @EXPORT = qw($PREFIX
                  $BINDIR $BINDIR_REL
                  $LIBDIR $LIBDIR_REL
                  $INCLUDEDIR $INCLUDEDIR_REL
                  $APPLINKDIR $APPLINKDIR_REL
                  $ENGINESDIR $ENGINESDIR_REL
                  $MODULESDIR $MODULESDIR_REL
                  $PKGCONFIGDIR $PKGCONFIGDIR_REL
                  $CMAKECONFIGDIR $CMAKECONFIGDIR_REL
                  $VERSION @LDLIBS);

our $PREFIX             = '/home/dionach/mp-dn/openssl';
our $BINDIR             = '/home/dionach/mp-dn/openssl/apps';
our $BINDIR_REL         = 'apps';
our $LIBDIR             = '/home/dionach/mp-dn/openssl';
our $LIBDIR_REL         = '.';
our $INCLUDEDIR         = '/home/dionach/mp-dn/openssl/include';
our $INCLUDEDIR_REL     = 'include';
our $APPLINKDIR         = '/home/dionach/mp-dn/openssl/ms';
our $APPLINKDIR_REL     = 'ms';
our $ENGINESDIR         = '/home/dionach/mp-dn/openssl/engines';
our $ENGINESDIR_REL     = 'engines';
our $MODULESDIR         = '/home/dionach/mp-dn/openssl/providers';
our $MODULESDIR_REL     = 'providers';
our $PKGCONFIGDIR       = '';
our $PKGCONFIGDIR_REL   = '';
our $CMAKECONFIGDIR     = '';
our $CMAKECONFIGDIR_REL = '';
our $VERSION            = '3.3.0-dev';
our @LDLIBS             =
    # Unix and Windows use space separation, VMS uses comma separation
    split(/ +| *, */, '-ldl -pthread ');

1;
