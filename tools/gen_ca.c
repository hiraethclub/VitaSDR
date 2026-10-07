/* Host-only generator: PEM CA bundle -> BearSSL trust-anchor C table.
 * Emits ca_bundle.c on stdout. Uses only BearSSL public API. */
#include "bearssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_dn[8192]; static size_t g_dnlen;
static void dn_append(void *ctx, const void *buf, size_t len){
    (void)ctx; if (g_dnlen+len<=sizeof g_dn){ memcpy(g_dn+g_dnlen,buf,len); g_dnlen+=len; }
}
static unsigned char g_der[1<<16]; static size_t g_derlen; static int g_cap;
static void der_sink(void *ctx,const void*buf,size_t len){
    (void)ctx; if(g_cap && g_derlen+len<=sizeof g_der){ memcpy(g_der+g_derlen,buf,len); g_derlen+=len; }
}
static void emit_blob(const char*name,const unsigned char*d,size_t n){
    printf("static const unsigned char %s[] = {",name);
    for(size_t i=0;i<n;i++){ if(i%12==0)printf("\n\t"); printf("0x%02X,",d[i]); }
    printf("\n};\n");
}
static int nta=0;
static void emit_ta(const br_x509_decoder_context*dc,const unsigned char*dn,size_t dnlen){
    const br_x509_pkey*pk=br_x509_decoder_get_pkey((br_x509_decoder_context*)dc);
    if(!pk) return;
    int ca=br_x509_decoder_isCA((br_x509_decoder_context*)dc);
    char b[32];
    snprintf(b,sizeof b,"DN%d",nta); emit_blob(b,dn,dnlen);
    if(pk->key_type==BR_KEYTYPE_RSA){
        snprintf(b,sizeof b,"RSA_N%d",nta); emit_blob(b,pk->key.rsa.n,pk->key.rsa.nlen);
        snprintf(b,sizeof b,"RSA_E%d",nta); emit_blob(b,pk->key.rsa.e,pk->key.rsa.elen);
    } else if(pk->key_type==BR_KEYTYPE_EC){
        snprintf(b,sizeof b,"EC_Q%d",nta); emit_blob(b,pk->key.ec.q,pk->key.ec.qlen);
    } else return;
    printf("/* ta %d: keytype=%d ca=%d */\n",nta,pk->key_type,ca);
    /* stash into arrays printed later via a marker file */
    fprintf(stderr,"%d %d %d %u\n",nta,pk->key_type,ca,
            pk->key_type==BR_KEYTYPE_EC?pk->key.ec.curve:0);
    nta++;
}

int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f){perror("open");return 1;}
    br_pem_decoder_context pc; br_pem_decoder_init(&pc);
    int incert=0;
    unsigned char buf[4096]; size_t rd;
    printf("/* Auto-generated from %s by gen_ca.c. Do not edit. */\n",argv[1]);
    printf("#include \"bearssl.h\"\n#include \"ca_bundle.h\"\n\n");
    /* collect metadata to build the TA array at the end */
    FILE*meta=tmpfile();
    while((rd=fread(buf,1,sizeof buf,f))>0){
        size_t off=0;
        while(off<rd){
            size_t c=br_pem_decoder_push(&pc,buf+off,rd-off); off+=c;
            switch(br_pem_decoder_event(&pc)){
            case BR_PEM_BEGIN_OBJ:
                if(strcmp(br_pem_decoder_name(&pc),"CERTIFICATE")==0 ||
                   strcmp(br_pem_decoder_name(&pc),"X509 CERTIFICATE")==0){
                    g_cap=1; g_derlen=0; br_pem_decoder_setdest(&pc,der_sink,NULL); incert=1;
                } else { g_cap=0; br_pem_decoder_setdest(&pc,NULL,NULL); }
                break;
            case BR_PEM_END_OBJ:
                if(incert){
                    br_x509_decoder_context dc; g_dnlen=0;
                    br_x509_decoder_init(&dc,dn_append,NULL);
                    br_x509_decoder_push(&dc,g_der,g_derlen);
                    const br_x509_pkey*pk=br_x509_decoder_get_pkey(&dc);
                    if(pk){
                        emit_ta(&dc,g_dn,g_dnlen);
                        int kt=pk->key_type, ca=br_x509_decoder_isCA(&dc);
                        unsigned cv=(kt==BR_KEYTYPE_EC)?pk->key.ec.curve:0;
                        fprintf(meta,"%d %d %d %u %u %u\n",nta-1,kt,ca,cv,
                            (unsigned)g_dnlen,
                            kt==BR_KEYTYPE_RSA?(unsigned)pk->key.rsa.nlen:(unsigned)pk->key.ec.qlen);
                    }
                    incert=0;
                }
                break;
            case BR_PEM_ERROR: fprintf(stderr,"PEM error\n"); break;
            }
        }
    }
    fclose(f);
    /* emit the TA array */
    printf("\nconst br_x509_trust_anchor VITASDR_TAs[] = {\n");
    rewind(meta); int idx,kt,ca; unsigned cv,dnl,kl;
    while(fscanf(meta,"%d %d %d %u %u %u",&idx,&kt,&ca,&cv,&dnl,&kl)==6){
        printf("\t{ { (unsigned char*)DN%d, sizeof DN%d }, %s,\n",idx,idx,
               ca?"BR_X509_TA_CA":"0");
        if(kt==BR_KEYTYPE_RSA){
            printf("\t  { BR_KEYTYPE_RSA, { .rsa = { (unsigned char*)RSA_N%d, sizeof RSA_N%d,"
                   " (unsigned char*)RSA_E%d, sizeof RSA_E%d } } } },\n",idx,idx,idx,idx);
        } else {
            printf("\t  { BR_KEYTYPE_EC, { .ec = { %u, (unsigned char*)EC_Q%d, sizeof EC_Q%d } } } },\n",
                   cv,idx,idx);
        }
    }
    printf("};\nconst size_t VITASDR_TAs_NUM = sizeof VITASDR_TAs / sizeof VITASDR_TAs[0];\n");
    fprintf(stderr,"generated %d trust anchors\n",nta);
    return 0;
}
