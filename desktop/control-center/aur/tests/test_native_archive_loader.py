"""Real loader process with a stub ALPM ABI; never GPG/native trust execution."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE=Path(__file__).resolve().parents[1]/'native_archive_loader.c'
HEADER=r'''
#ifndef TEST_ALPM_H
#define TEST_ALPM_H
#define ALPM_SIG_PACKAGE 1
#define ALPM_PKG_VALIDATION_SIGNATURE 2
typedef int alpm_errno_t;
typedef union { struct { int answer; } any; } alpm_question_t;
typedef struct alpm_handle_t alpm_handle_t;
typedef struct alpm_pkg_t alpm_pkg_t;
alpm_handle_t *alpm_initialize(const char *,const char *,alpm_errno_t *);
int alpm_option_set_gpgdir(alpm_handle_t *,const char *);
int alpm_option_set_default_siglevel(alpm_handle_t *,int);
int alpm_option_set_local_file_siglevel(alpm_handle_t *,int);
int alpm_option_set_questioncb(alpm_handle_t *,void (*)(void *,alpm_question_t *),void *);
int alpm_pkg_load(alpm_handle_t *,const char *,int,int,alpm_pkg_t **);
int alpm_pkg_get_validation(alpm_pkg_t *);
int alpm_pkg_free(alpm_pkg_t *);
int alpm_release(alpm_handle_t *);
#endif
'''
STUB=r'''
#include "alpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
struct alpm_handle_t {int calls;};struct alpm_pkg_t {int valid;};
static struct alpm_handle_t handle;
static struct alpm_pkg_t package;
static void (*question_cb)(void *,alpm_question_t *);
static void *question_context;
alpm_handle_t *alpm_initialize(const char *root,const char *db,alpm_errno_t *error) {
    (void)error;
    if(strncmp(root,"/proc/self/fd/",14) || strncmp(db,"/proc/self/fd/",14))return NULL;
    printf("INIT_DOMAIN=%s\n",getenv("GNUPGHOME"));
    printf("INHERITED_GPGME_DEBUG=%s\n",getenv("GPGME_DEBUG")?"present":"absent");
    return &handle;
}
int alpm_option_set_gpgdir(alpm_handle_t *h,const char *path) {
    if(++h->calls!=1 || strcmp(path,"/etc/zethropol-update/aur-native-trust"))return -1;
    printf("FIXED_DOMAIN=%s\n",path);return 0;
}
int alpm_option_set_default_siglevel(alpm_handle_t *h,int policy) {(void)h;printf("DEFAULT_POLICY=%d\n",policy);return policy==1?0:-1;}
int alpm_option_set_local_file_siglevel(alpm_handle_t *h,int policy) {(void)h;printf("LOCAL_POLICY=%d\n",policy);return policy==1?0:-1;}
int alpm_option_set_questioncb(alpm_handle_t *h,void (*cb)(void *,alpm_question_t *),void *context) {(void)h;question_cb=cb;question_context=context;return 0;}
int alpm_pkg_load(alpm_handle_t *h,const char *path,int full,int policy,alpm_pkg_t **out) {
    if(h->calls!=1 || full!=1 || policy!=1)return -1;
    FILE *f=fopen(path,"rb");if(!f)return -1;int c=fgetc(f);fclose(f);
    char sigpath[512];snprintf(sigpath,sizeof(sigpath),"%s.sig",path);
    f=fopen(sigpath,"rb");if(!f)return -1;int sig=fgetc(f);fclose(f);
    if(c=='R' || sig!='S')return -1;
    if(c=='C') {
        char root[512],moved[520];ssize_t n;
        /* Discover the parent of the staging directory through its pinned FD. */
        char stage[512];snprintf(stage,sizeof(stage),"%s",path);
        char *end=strrchr(stage,'/');if(!end)return -1;*end=0;
        n=readlink(stage,root,sizeof(root)-1);if(n<0)return -1;root[n]=0;
        end=strrchr(root,'/');if(!end)return -1;*end=0;
        snprintf(moved,sizeof(moved),"%s.moved",root);
        if(rename(root,moved) || mkdir(root,0700))return -1;
    }
    if(c=='Q') {alpm_question_t q={0};q.any.answer=1;question_cb(question_context,&q);printf("QUESTION_ANSWER=%d\n",q.any.answer);}
    package.valid=c=='V' || c=='C' || c=='Q';*out=&package;return 0;
}
int alpm_pkg_get_validation(alpm_pkg_t *p) {return p->valid?ALPM_PKG_VALIDATION_SIGNATURE:0;}
int alpm_pkg_free(alpm_pkg_t *p) {(void)p;return 0;}
int alpm_release(alpm_handle_t *h) {(void)h;return 0;}
'''

@unittest.skipUnless(shutil.which('cc') and hasattr(os,'memfd_create') and os.getuid()!=0,'unprivileged Linux compiler fixture required')
class NativeLoader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.build=Path(cls.temp.name)
        (cls.build/'alpm.h').write_text(HEADER);(cls.build/'stub.c').write_text(STUB)
        cls.binary=cls.build/'native-loader'
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I',str(cls.build),str(SOURCE),str(cls.build/'stub.c'),'-o',str(cls.binary)],check=True,capture_output=True)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def memfd(self,data,sealed=True):
        fd=os.memfd_create('unsigned-fixture',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING);self.addCleanup(os.close,fd);os.write(fd,data)
        if sealed:fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL)
        return fd
    def invoke(self,package=b'V unsigned test bytes',signature=b'S stub signature',sealed=True,extra=(),same=False,duplicate=False,same_storage=False,root_mode=0o700,alias=False):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);root=p/'root';db=p/'db';root.mkdir(mode=root_mode);os.chmod(root,root_mode);db.mkdir(mode=0o700);os.chmod(db,0o700)
            if alias:(p/'alias').symlink_to(root);root=p/'alias'
            if same_storage:db=root
            package_fd=self.memfd(package,sealed);signature_fd=package_fd if same else self.memfd(signature)
            if duplicate:signature_fd=os.dup(package_fd);self.addCleanup(os.close,signature_fd)
            result=subprocess.run([str(self.binary),str(root),str(db),str(package_fd),str(signature_fd),*extra],pass_fds=(package_fd,signature_fd),capture_output=True,text=True,timeout=5,
                                  env={**os.environ,'GNUPGHOME':'/tmp/untrusted-client-domain','GPGME_DEBUG':'unsafe-caller-value'})
            files=sorted(n.name for n in (p/'root').iterdir())
            return result,files
    def test_fixed_domain_precedes_native_load_and_strips_caller_environment(self):
        result,files=self.invoke()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('INIT_DOMAIN=/etc/zethropol-update/aur-native-trust',result.stdout)
        self.assertIn('FIXED_DOMAIN=/etc/zethropol-update/aur-native-trust',result.stdout)
        self.assertIn('INHERITED_GPGME_DEBUG=absent',result.stdout)
        self.assertIn('DEFAULT_POLICY=1',result.stdout);self.assertIn('LOCAL_POLICY=1',result.stdout)
        self.assertNotIn('untrusted-client-domain',result.stdout)
        self.assertEqual(len(files),1);self.assertTrue(files[0].startswith('.native-archive-'))
        self.assertIn('no install grant',result.stdout)
    def test_native_rejection_or_unsigned_validation_never_becomes_success(self):
        for prefix in (b'R',b'U'):
            result,files=self.invoke(package=prefix+b' unsigned test bytes')
            self.assertEqual(result.returncode,1);self.assertNotIn('VERIFIED-ONLY',result.stdout);self.assertEqual(len(files),1)
    def test_unsealed_empty_alias_fd_or_extra_domain_arg_rejected_before_staging(self):
        for options in ({'sealed':False},{'package':b''},{'signature':b''},{'same':True},{'duplicate':True},{'same_storage':True},{'extra':('/tmp/client-domain',)}):
            with self.subTest(options=options):
                result,files=self.invoke(**options);self.assertEqual(result.returncode,2);self.assertEqual(files,[])
    def test_nonprivate_or_symlink_storage_is_rejected(self):
        for options in ({'root_mode':0o755},{'alias':True}):
            result,files=self.invoke(**options);self.assertEqual(result.returncode,2);self.assertEqual(files,[])
    def test_native_question_is_denied_and_cannot_authorize_import_or_load(self):
        result,_=self.invoke(package=b'Q native import question');self.assertEqual(result.returncode,1)
        self.assertIn('QUESTION_ANSWER=0',result.stdout);self.assertNotIn('VERIFIED-ONLY',result.stdout)
    def test_staged_signature_name_and_storage_identity_are_required(self):
        for options in ({'signature':b'not an accepted stub signature'},{'package':b'C swap disposable root'}):
            result,_=self.invoke(**options);self.assertEqual(result.returncode,1);self.assertNotIn('VERIFIED-ONLY',result.stdout)
    def test_real_source_has_one_domain_assignment_and_no_transaction_or_cleanup_path(self):
        text=SOURCE.read_text();self.assertEqual(text.count('alpm_option_set_gpgdir('),1)
        for forbidden in ('alpm_trans_init(','alpm_trans_prepare(','alpm_trans_commit(','alpm_register_syncdb(','unlink(','rename(','system(','execv('):
            self.assertNotIn(forbidden,text)
        self.assertNotIn('ALPM_SIG_PACKAGE_OPTIONAL',text);self.assertNotIn('ALPM_SIG_PACKAGE_MARGINAL_OK',text);self.assertNotIn('ALPM_SIG_PACKAGE_UNKNOWN_OK',text)

if __name__=='__main__':unittest.main()
