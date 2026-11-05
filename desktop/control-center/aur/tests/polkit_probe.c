/* Read-only authorization probe for the actual unique system-bus peer.
 * Does not call Update, request interaction or perform an installation.
 */
#include <systemd/sd-bus.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
static int identity(sd_bus *bus,const char *name,const char *method,uint32_t expected) {
    sd_bus_message *reply=NULL; sd_bus_error error=SD_BUS_ERROR_NULL; uint32_t value;
    int r=sd_bus_call_method(bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",method,&error,&reply,"s",name);
    if(r>=0) r=sd_bus_message_read(reply,"u",&value);
    int ok=r>0 && value==expected;
    sd_bus_error_free(&error);sd_bus_message_unref(reply);return ok;
}
int main(void) {
    sd_bus *bus=NULL;sd_bus_message *call=NULL,*reply=NULL;sd_bus_error error=SD_BUS_ERROR_NULL;
    const char *name=NULL;int result=1,authorized=0,challenge=0;
    if(getuid()==0 || sd_bus_open_system(&bus)<0 || sd_bus_get_unique_name(bus,&name)<0) goto done;
    if(!identity(bus,name,"GetConnectionUnixUser",getuid()) || !identity(bus,name,"GetConnectionUnixProcessID",getpid())) goto done;
    if(sd_bus_message_new_method_call(bus,&call,"org.freedesktop.PolicyKit1","/org/freedesktop/PolicyKit1/Authority","org.freedesktop.PolicyKit1.Authority","CheckAuthorization")<0) goto done;
    if(sd_bus_message_append(call,"(sa{sv})sa{ss}us","system-bus-name",1,"name","s",name,"org.zethropol.update.install",0,0,"")<0) goto done;
    if(sd_bus_call(bus,call,10000000,&error,&reply)<0) goto done;
    if(sd_bus_message_enter_container(reply,'r',"bba{ss}")<0 || sd_bus_message_read(reply,"bb",&authorized,&challenge)<0) goto done;
    if(!identity(bus,name,"GetConnectionUnixUser",getuid()) || !identity(bus,name,"GetConnectionUnixProcessID",getpid())) goto done;
    printf("ACTUAL_PEER uid=%u pid=%u name=%s authorized=%d challenge=%d interaction=disabled\n",getuid(),getpid(),name,authorized,challenge);
    if(authorized) { fputs("Existing policy authorized this peer; denial not proven\n",stderr);goto done; }
    puts("PASS actual PolicyKit noninteractive denial; no Update method called");result=0;
done:
    if(result && error.message) fprintf(stderr,"PolicyKit probe failed: %s\n",error.message);
    sd_bus_error_free(&error);sd_bus_message_unref(call);sd_bus_message_unref(reply);sd_bus_unref(bus);return result;
}
