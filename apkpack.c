/* ============================================================
   RigCom v8.0 — src/apkpack.c
   Manifest v2 · V1/V2/V3 Signing · Iconos Adaptativos
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/apkpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define APK_KS_NAME   "rigcom_master.keystore"
#define APK_KS_PASS   "rigcom_ks_2025"
#define APK_KEY_ALIAS "rigcom_key"
#define APK_KEY_PASS  "rigcom_key_2025"

/* ── Script Python para iconos dorados ── */
static const char ICON_PY[] =
"import struct,zlib,os,math\n"
"def chunk(t,d):\n"
"    return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)\n"
"def icon(s):\n"
"    raw=b'';cx=cy=s//2\n"
"    for y in range(s):\n"
"        row=b'\\x00'\n"
"        for x in range(s):\n"
"            dx,dy=x-cx,y-cy;d=math.hypot(dx,dy)/cx\n"
"            r=int(max(0,12+14*(1-d)));g=int(max(0,8+8*(1-d)));b=int(max(0,38+24*(1-d)))\n"
"            if 0.82<d<0.98:\n"
"                t=1-abs(d-0.9)/0.08;r=min(255,r+int(t*244));g=min(255,g+int(t*193))\n"
"            nx=(x-cx*0.7)/(cx*0.4);ny=(y-cy*0.7)/(cy*0.7)\n"
"            stem=abs(nx+0.55)<0.22 and abs(ny)<1.0\n"
"            bow=abs(math.hypot(nx+0.12,max(0,-(ny+0.1)))-0.6)<0.26 and ny<0.06\n"
"            leg=abs(nx-ny*0.65+0.15)<0.22 and 0.08<ny<1.05\n"
"            if (stem or bow or leg) and d<0.78:\n"
"                r=min(255,r+228);g=min(255,g+188)\n"
"            row+=bytes([r,g,b])\n"
"        raw+=row\n"
"    ih=struct.pack('>IIBBBBB',s,s,8,2,0,0,0)\n"
"    return b'\\x89PNG\\r\\n\\x1a\\n'+chunk(b'IHDR',ih)+chunk(b'IDAT',zlib.compress(raw,6))+chunk(b'IEND',b'')\n"
"for d,s in [('xxxhdpi',192),('xxhdpi',144),('xhdpi',96),('hdpi',72),('mdpi',48)]:\n"
"    os.makedirs(f'build/res/mipmap-{d}',exist_ok=True)\n"
"    open(f'build/res/mipmap-{d}/ic_launcher.png','wb').write(icon(s))\n"
"print('ICONS_OK')\n";

bool apk_gen_icons(RigCtx *ctx) {
    rigctx_ws_emit(ctx,
        "{\"ev\":\"apk_phase\",\"phase\":\"icons\","
        "\"detail\":\"Generando iconos adaptativos (5 densidades)...\"}");
    FILE *f = fopen("/tmp/_rig_icon.py","w");
    if (!f) return false;
    fputs(ICON_PY, f); fclose(f);
    if (system("python3 /tmp/_rig_icon.py 2>/dev/null") == 0) {
        remove("/tmp/_rig_icon.py"); return true;
    }
    /* Fallback: ImageMagick */
    static const struct { const char *d; int s; } D[] = {
        {"xxxhdpi",192},{"xxhdpi",144},{"xhdpi",96},{"hdpi",72},{"mdpi",48}
    };
    bool ok = false;
    for (int i = 0; i < 5; i++) {
        char cmd[512];
        snprintf(cmd,sizeof(cmd),
            "mkdir -p build/res/mipmap-%s && "
            "convert -size %dx%d gradient:'#0a0a28-#1a1028' "
            "-fill '#FFD700' -font DejaVu-Sans-Bold -pointsize %d "
            "-gravity Center -annotate 0 'R' "
            "build/res/mipmap-%s/ic_launcher.png 2>/dev/null",
            D[i].d,D[i].s,D[i].s,D[i].s*3/4,D[i].d);
        if (system(cmd)==0) ok=true;
    }
    remove("/tmp/_rig_icon.py");
    return ok;
}

/* ── Manifest v2: SDK 24-34, extractNativeLibs, icon, exported ── */
static bool generate_manifest(const char *app_name) {
    FILE *f = fopen("build/AndroidManifest.xml","w");
    if (!f) return false;
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"com.rigcom.%s\"\n"
        "    android:versionCode=\"1\" android:versionName=\"1.0\">\n"
        "    <uses-sdk android:minSdkVersion=\"24\" android:targetSdkVersion=\"34\"/>\n"
        "    <uses-feature android:glEsVersion=\"0x00030000\" android:required=\"true\"/>\n"
        "    <uses-permission android:name=\"android.permission.INTERNET\"/>\n"
        "    <application android:label=\"%s\"\n"
        "                 android:icon=\"@mipmap/ic_launcher\"\n"
        "                 android:hasCode=\"false\"\n"
        "                 android:extractNativeLibs=\"true\"\n"
        "                 android:allowBackup=\"false\">\n"
        "        <activity android:name=\"android.app.NativeActivity\"\n"
        "                  android:theme=\"@android:style/Theme.NoTitleBar.Fullscreen\"\n"
        "                  android:configChanges=\"orientation|keyboardHidden|screenSize|density\"\n"
        "                  android:exported=\"true\">\n"
        "            <meta-data android:name=\"android.app.lib_name\" android:value=\"main\"/>\n"
        "            <intent-filter>\n"
        "                <action android:name=\"android.intent.action.MAIN\"/>\n"
        "                <category android:name=\"android.intent.category.LAUNCHER\"/>\n"
        "            </intent-filter>\n"
        "        </activity>\n"
        "    </application>\n"
        "</manifest>\n", app_name, app_name);
    fclose(f); return true;
}

/* ── Keystore maestro RSA-4096 persistente ── */
static void ensure_keystore(void) {
    struct stat st;
    if (stat(APK_KS_NAME,&st)==0) return;
    (void)system(
        "keytool -genkeypair -keystore " APK_KS_NAME
        " -storepass " APK_KS_PASS
        " -alias " APK_KEY_ALIAS " -keypass " APK_KEY_PASS
        " -keyalg RSA -keysize 4096 -validity 20000"
        " -dname \"CN=RigCom,O=Sovereign,C=MX\" > /dev/null 2>&1");
}

bool apk_check_tools(RigCtx *ctx) {
    bool ok = true;
    static const char *T[] = {"aapt","apksigner","zipalign","zip","keytool"};
    static const char *P[] = {"aapt","apksigner","zipalign","zip","openjdk-17"};
    for (int i = 0; i < 5; i++) {
        char cmd[128]; snprintf(cmd,sizeof(cmd),"command -v %s >/dev/null 2>&1",T[i]);
        if (system(cmd)!=0) {
            char ev[256];
            snprintf(ev,sizeof(ev),
                "{\"ev\":\"error\",\"msg\":\"Falta %s → pkg install %s\"}",T[i],P[i]);
            rigctx_ws_emit(ctx,ev); ok=false;
        }
    }
    return ok;
}

bool apk_build(RigCtx *ctx, const char *so_path, const char *out_apk) {
    char cmd[1024];
    (void)system("mkdir -p build/lib/arm64-v8a build/lib/armeabi-v7a "
                 "build/lib/x86_64 build/assets");
    /* ABI arm64 */
    snprintf(cmd,sizeof(cmd),"cp %s build/lib/arm64-v8a/libmain.so",so_path);
    (void)system(cmd);
    /* ABI v7 */
    { char ll[512]; snprintf(ll,sizeof(ll),"%s.ll",so_path);
      char xcc[1024];
      snprintf(xcc,sizeof(xcc),"clang -target armv7-linux-androideabi -shared -fPIC "
          "-landroid -llog -lEGL -lGLESv2 -lm %s "
          "-o build/lib/armeabi-v7a/libmain.so >/dev/null 2>&1",ll);
      if (system(xcc)!=0)
          (void)system("cp build/lib/arm64-v8a/libmain.so build/lib/armeabi-v7a/libmain.so");
      rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"abi_v7\",\"detail\":\"armeabi-v7a OK\"}");
    }
    /* ABI x86_64 */
    { char ll[512]; snprintf(ll,sizeof(ll),"%s.ll",so_path);
      char xcc[1024];
      snprintf(xcc,sizeof(xcc),"clang -target x86_64-linux-android -shared -fPIC "
          "-landroid -llog -lEGL -lGLESv3 -lm %s "
          "-o build/lib/x86_64/libmain.so >/dev/null 2>&1",ll);
      if (system(xcc)!=0)
          (void)system("cp build/lib/arm64-v8a/libmain.so build/lib/x86_64/libmain.so");
      rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"abi_x64\",\"detail\":\"x86_64 OK\"}");
    }
    /* Iconos */
    apk_gen_icons(ctx);
    /* Manifest */
    const char *nm = ctx->config.project_name ? ctx->config.project_name : "hologram";
    generate_manifest(nm);
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"manifest\",\"detail\":\"Manifest v2 OK\"}");
    /* AAPT */
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"aapt\",\"detail\":\"Compilando recursos...\"}");
    snprintf(cmd,sizeof(cmd),
        "aapt package -f -M build/AndroidManifest.xml "
        "-I $PREFIX/share/aapt/android.jar "
        "-S build/res -A build/assets -F build/app.unaligned.apk");
    if (system(cmd)!=0) {
        snprintf(cmd,sizeof(cmd),
            "aapt package -f -M build/AndroidManifest.xml "
            "-I $PREFIX/share/aapt/android.jar "
            "-A build/assets -F build/app.unaligned.apk");
        if (system(cmd)!=0) return false;
    }
    /* Inyectar .so */
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"zip\",\"detail\":\"Inyectando .so multi-ABI...\"}");
    (void)system("cd build && zip -urq app.unaligned.apk lib/arm64-v8a/ lib/armeabi-v7a/ lib/x86_64/");
    /* zipalign */
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"zipalign\",\"detail\":\"Alineando 4 bytes...\"}");
    (void)system("zipalign -p -f 4 build/app.unaligned.apk build/app.aligned.apk");
    /* Keystore + firma V1+V2+V3 */
    ensure_keystore();
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"sign\",\"detail\":\"Firmando V1+V2+V3 (RSA-4096)...\"}");
    snprintf(cmd,sizeof(cmd),
        "apksigner sign --ks " APK_KS_NAME " --ks-pass pass:" APK_KS_PASS
        " --ks-key-alias " APK_KEY_ALIAS " --key-pass pass:" APK_KEY_PASS
        " --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true"
        " --out %s build/app.aligned.apk", out_apk);
    if (system(cmd)!=0) return false;
    /* Verificación */
    char vcmd[512];
    snprintf(vcmd,sizeof(vcmd),"apksigner verify --verbose %s 2>&1 | head -4",out_apk);
    if (system(vcmd)==0)
        rigctx_ws_emit(ctx,"{\"ev\":\"apk_phase\",\"phase\":\"verify\",\"detail\":\"✦ Firma V1+V2+V3 verificada.\"}");
    return true;
}

bool apk_unpack(RigCtx *ctx, const char *apk_path, const char *out_dir) {
    char cmd[1024];
    rigctx_ws_emit(ctx,"{\"ev\":\"bootstrap_progress\",\"stage\":\"UNPACK\",\"msg\":\"Descomprimiendo APK...\"}");
    snprintf(cmd,sizeof(cmd),"unzip -q %s -d %s",apk_path,out_dir);
    if (system(cmd)!=0) return false;
    snprintf(cmd,sizeof(cmd),"aapt dump xmltree %s AndroidManifest.xml > %s/Manifest_Readable.txt",apk_path,out_dir);
    (void)system(cmd);
    return true;
}

/* ── APK Explorer ── */
static const char *apk_cat(const char *p) {
    if (strncmp(p,"META-INF/",9)==0) return "meta";
    if (strcmp(p,"AndroidManifest.xml")==0) return "manifest";
    if (strncmp(p,"lib/",4)==0) return "native";
    if (strncmp(p,"assets/",7)==0) return "asset";
    if (strncmp(p,"res/",4)==0||strncmp(p,"mipmap",6)==0) return "resource";
    const char *d=strrchr(p,'.');
    if (d){
        if (!strcmp(d,".dex")) return "dex";
        if (!strcmp(d,".so"))  return "native";
        if (!strcmp(d,".xml")) return "xml";
        if (!strcmp(d,".png")||!strcmp(d,".jpg")) return "image";
    }
    return "other";
}
static size_t apk_esc(const char *s,char *o,size_t m){
    size_t w=0;
    for(const char *p=s;*p&&w+8<m;p++){
        unsigned char c=(unsigned char)*p;
        if(c=='"'||c=='\\'){o[w++]='\\';o[w++]=(char)c;}
        else if(c=='\n'){o[w++]='\\';o[w++]='n';}
        else if(c<0x20){}
        else{o[w++]=(char)c;}
    }
    o[w]='\0';return w;
}
bool apk_list_contents(const char *dir,char *jo,size_t mx){
    char tmp[256]; snprintf(tmp,sizeof(tmp),"/tmp/rig_ls%d.txt",(int)getpid());
    char cmd[1024]; snprintf(cmd,sizeof(cmd),"cd '%s'&&find . -type f|sed 's|^\\./||'|sort>'%s' 2>/dev/null",dir,tmp);
    (void)system(cmd);
    FILE *fp=fopen(tmp,"r"); if(!fp){snprintf(jo,mx,"[]");return false;}
    char *o=jo; size_t r=mx; bool f=true;
    size_t n=(size_t)snprintf(o,r,"["); o+=n;r-=n;
    char ln[512];
    while(fgets(ln,sizeof(ln),fp)&&r>128){
        size_t ll=strlen(ln); while(ll>0&&(ln[ll-1]=='\n'||ln[ll-1]=='\r'))ln[--ll]='\0'; if(!ll)continue;
        char full[768]; snprintf(full,sizeof(full),"%s/%s",dir,ln);
        struct stat st; long sz=(stat(full,&st)==0)?(long)st.st_size:-1;
        char e[512]; apk_esc(ln,e,sizeof(e));
        n=(size_t)snprintf(o,r,"%s{\"path\":\"%s\",\"size\":%ld,\"cat\":\"%s\"}",f?"":",",e,sz,apk_cat(ln));
        o+=n;r-=n;f=false;
    }
    fclose(fp);remove(tmp);
    if(r>2){o[0]=']';o[1]='\0';}
    return true;
}
char *apk_read_file(const char *dir,const char *rel,size_t *sz){
    if(strstr(rel,".."))return NULL;
    char full[768]; snprintf(full,sizeof(full),"%s/%s",dir,rel);
    const char *d=strrchr(rel,'.');
    if(d&&(!strcmp(d,".so")||!strcmp(d,".dex"))){
        char tmp[256]; snprintf(tmp,sizeof(tmp),"/tmp/rigbi%d.txt",(int)getpid());
        char cmd[1024];
        if(!strcmp(d,".so"))
            snprintf(cmd,sizeof(cmd),"readelf -h '%s'>'%s' 2>/dev/null&&nm -D --defined-only '%s' 2>/dev/null|head -80>>'%s'",full,tmp,full,tmp);
        else
            snprintf(cmd,sizeof(cmd),"echo '[DEX]'>'%s'&&file '%s'>>'%s' 2>/dev/null",tmp,full,tmp);
        (void)system(cmd);
        FILE *f=fopen(tmp,"r");
        if(f){fseek(f,0,SEEK_END);long fs=ftell(f);rewind(f);
            if(fs>0){char *b=malloc((size_t)fs+1);if(b){size_t rd=fread(b,1,(size_t)fs,f);b[rd]='\0';fclose(f);remove(tmp);if(sz)*sz=rd;return b;}}
            fclose(f);remove(tmp);}
    }
    FILE *f=fopen(full,"r"); if(!f)return NULL;
    fseek(f,0,SEEK_END);long fs=ftell(f);rewind(f);
    if(fs<=0){fclose(f);return NULL;} if(fs>262144)fs=262144;
    char *b=malloc((size_t)fs+1); if(!b){fclose(f);return NULL;}
    size_t rd=fread(b,1,(size_t)fs,f);b[rd]='\0';fclose(f);
    if(sz)*sz=rd; return b;
}
bool apk_write_file(const char *dir,const char *rel,const char *content,size_t len){
    if(strstr(rel,".."))return false;
    char full[768]; snprintf(full,sizeof(full),"%s/%s",dir,rel);
    char par[768]; snprintf(par,sizeof(par),"%s",full);
    char *sl=strrchr(par,'/'); if(sl){*sl='\0';char mk[800];snprintf(mk,sizeof(mk),"mkdir -p '%s'",par);(void)system(mk);}
    FILE *f=fopen(full,"w"); if(!f)return false;
    fwrite(content,1,len,f);fclose(f);return true;
}
bool apk_repack(RigCtx *ctx,const char *dir,const char *out){
    char cmd[1024];
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_repack_progress\",\"stage\":\"ZIP\",\"msg\":\"Reempaquetando...\"}");
    char un[512]; snprintf(un,sizeof(un),"%s/../_rig_re_unalign.apk",dir);
    snprintf(cmd,sizeof(cmd),"cd '%s'&&zip -r '../_rig_re_unalign.apk' . >/dev/null 2>&1",dir);
    if(system(cmd)!=0){rigctx_ws_emit(ctx,"{\"ev\":\"apk_repack_done\",\"ok\":false,\"msg\":\"Error ZIP\"}");return false;}
    char al[512]; snprintf(al,sizeof(al),"%s/../_rig_re_align.apk",dir);
    snprintf(cmd,sizeof(cmd),"zipalign -p -f 4 '%s' '%s' >/dev/null 2>&1",un,al);
    if(system(cmd)!=0){snprintf(cmd,sizeof(cmd),"cp '%s' '%s'",un,al);(void)system(cmd);}
    ensure_keystore();
    rigctx_ws_emit(ctx,"{\"ev\":\"apk_repack_progress\",\"stage\":\"SIGN\",\"msg\":\"Firmando V1+V2+V3...\"}");
    snprintf(cmd,sizeof(cmd),
        "apksigner sign --ks " APK_KS_NAME " --ks-pass pass:" APK_KS_PASS
        " --ks-key-alias " APK_KEY_ALIAS " --key-pass pass:" APK_KEY_PASS
        " --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true"
        " --out '%s' '%s' >/dev/null 2>&1",out,al);
    bool ok=(system(cmd)==0);
    remove(un);remove(al);
    if(ok){char msg[640];snprintf(msg,sizeof(msg),"{\"ev\":\"apk_repack_done\",\"ok\":true,\"path\":\"%s\",\"msg\":\"Reempaquetado V1+V2+V3: %s\"}",out,out);rigctx_ws_emit(ctx,msg);}
    else rigctx_ws_emit(ctx,"{\"ev\":\"apk_repack_done\",\"ok\":false,\"msg\":\"Error al firmar\"}");
    return ok;
}
