/* Read-only native PackageRequired/TrustedOnly probe. Disposable root, DB and
 * public GPG snapshot supplied by test harness. No transaction or callbacks. */
#include <alpm.h>
#include <stdio.h>
int main(int argc,char **argv) {
    if(argc!=5)return 2;
    alpm_errno_t error;alpm_handle_t *h=alpm_initialize(argv[1],argv[2],&error);
    if(!h){fprintf(stderr,"alpm init: %s\n",alpm_strerror(error));return 2;}
    if(alpm_option_set_gpgdir(h,argv[3])<0){alpm_release(h);return 2;}
    alpm_pkg_t *package=NULL;
    int result=alpm_pkg_load(h,argv[4],1,ALPM_SIG_PACKAGE,&package);
    if(result<0){fprintf(stderr,"native signature rejected: %s\n",alpm_strerror(alpm_errno(h)));alpm_release(h);return 1;}
    int verified=alpm_pkg_get_validation(package)&ALPM_PKG_VALIDATION_SIGNATURE;
    if(verified)printf("verified %s %s\n",alpm_pkg_get_name(package),alpm_pkg_get_version(package));
    alpm_pkg_free(package);alpm_release(h);return verified?0:1;
}
