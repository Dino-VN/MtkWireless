// Read the public BSD identity used by network services; no root required.
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_media.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0) { perror("socket"); return 1; }
    struct ifreq request={0};
    strlcpy(request.ifr_name,"awdl0",sizeof(request.ifr_name));
    if(ioctl(fd,SIOCGIFFUNCTIONALTYPE,&request)<0) {
        perror("awdl0 functional type"); close(fd); return 1;
    }
    unsigned type=request.ifr_ifru.ifru_functional_type;
    struct ifmediareq media={0};
    strlcpy(media.ifm_name,"awdl0",sizeof(media.ifm_name));
    if(ioctl(fd,SIOCGIFMEDIA,&media)<0) {
        perror("awdl0 media"); close(fd); return 1;
    }
    close(fd);
    printf("awdl0 functional type=%u (expected 4: Wi-Fi AWDL), media=%#x active=%#x status=%#x\n",
           type,media.ifm_current,media.ifm_active,media.ifm_status);
    if(type!=4 || IFM_TYPE(media.ifm_current)!=IFM_IEEE80211) {
        puts("FAIL: BSD identity is not Wi-Fi AWDL."); return 1;
    }
    puts("PASS: BSD identifies awdl0 as Wi-Fi AWDL.");
    return 0;
}
