/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014 David Quintana [gigaherz]
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdarg.h>
#include "pcap.h"
#include "pcap_io.h"

#include "DEV9.h"
#include "net.h"

#ifdef _WIN32
#include <Iphlpapi.h>
#elif defined(__linux__)
#include <sys/ioctl.h>
#include <net/if.h>
#endif

#ifndef PCAP_NETMASK_UNKNOWN
#define PCAP_NETMASK_UNKNOWN    0xffffffff
#endif

mac_address virtual_mac = { 0x00,0x24,0x8D,0x63, 0x30, 0x31 }; // first three recognized by Xlink as Sony PS2
mac_address broadcast_mac = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

pcap_t *adhandle;
int pcap_io_running=0;
extern u8 eeprom[];
char errbuf[PCAP_ERRBUF_SIZE];

char namebuff[256];

pcap_dumper_t *dump_pcap;

mac_address host_mac = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Fetches the MAC address and prints it
int GetMACAddress(char *adapter, mac_address* addr)
{
	int retval = 0;
#ifdef _WIN32
	static IP_ADAPTER_INFO AdapterInfo[128];       // Allocate information
												 // for up to 128 NICs
	static PIP_ADAPTER_INFO pAdapterInfo;
	ULONG dwBufLen = sizeof(AdapterInfo);	// Save memory size of buffer

	DWORD dwStatus = GetAdaptersInfo(      // Call GetAdapterInfo
	AdapterInfo,                 // [out] buffer to receive data
	&dwBufLen);                  // [in] size of receive data buffer
	if(dwStatus != ERROR_SUCCESS)    // Verify return value is
		return 0;                       // valid, no buffer overflow

	pAdapterInfo = AdapterInfo; // Contains pointer to
											   // current adapter info
	do {
		if(strcmp(pAdapterInfo->AdapterName,adapter)==0)
		{
			memcpy(addr,pAdapterInfo->Address,6);
			return 1;
		}

		pAdapterInfo = pAdapterInfo->Next;    // Progress through
	}
	while(pAdapterInfo);                    // Terminate if last adapter
#elif defined(__linux__)
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	strcpy(ifr.ifr_name, adapter);
	if (0 == ioctl(fd, SIOCGIFHWADDR, &ifr))
	{
		retval = 1;
		memcpy(addr,ifr.ifr_hwaddr.sa_data,6);
	}
	else
	{
		SysMessage("Could not get MAC address for adapter: %s", adapter);
	}
	close(fd);
#endif
	return retval;
}

int pcap_io_init(char *adapter)
{
	struct bpf_program fp;
	char filter[1024] = "ether broadcast or ether dst ";
	int dlt;
	char *dlt_name;
	emu_printf("Opening adapter '%s'...",adapter);
	u16 checksum;
	GetMACAddress(adapter,&host_mac);
	
	//Lets take the hosts last 2 bytes to make it unique on Xlink
	virtual_mac.bytes[4] = host_mac.bytes[4];
	virtual_mac.bytes[5] = host_mac.bytes[5];

	for(int ii=0; ii<6; ii++)
		eeprom[ii] = virtual_mac.bytes[ii];

	//The checksum seems to be all the values of the mac added up in 16bit chunks
	checksum = (dev9.eeprom[0] + dev9.eeprom[1] + dev9.eeprom[2]) & 0xffff;

	dev9.eeprom[3] = checksum;

	/* Open the adapter */
	if ((adhandle= pcap_open_live(adapter,	// name of the device
							 65536,			// portion of the packet to capture. 
											// 65536 grants that the whole packet will be captured on all the MACs.
							 1,				// promiscuous for Xlink usage
							 1,			// read timeout
							 errbuf			// error buffer
							 )) == NULL)
	{
		fprintf(stderr, "%s", errbuf);
		fprintf(stderr,"\nUnable to open the adapter. %s is not supported by pcap\n", adapter);
		return -1;
	}
	char virtual_mac_str[18];
	sprintf(virtual_mac_str, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x" , virtual_mac.bytes[0], virtual_mac.bytes[1], virtual_mac.bytes[2], virtual_mac.bytes[3], virtual_mac.bytes[4], virtual_mac.bytes[5]);
	strcat(filter,virtual_mac_str);
//	fprintf(stderr, "Trying pcap filter: %s\n", filter);

	if(pcap_compile(adhandle,&fp,filter,1,PCAP_NETMASK_UNKNOWN) == -1)
	{
		fprintf(stderr,"Error calling pcap_compile: %s\n", pcap_geterr(adhandle));
		return -1;
	}

	if(pcap_setfilter(adhandle,&fp) == -1)
	{
		fprintf(stderr,"Error setting filter: %s\n", pcap_geterr(adhandle));
		return -1;
	}
	

	dlt = pcap_datalink(adhandle);
	dlt_name = (char*)pcap_datalink_val_to_name(dlt);

	fprintf(stderr,"Device uses DLT %d: %s\n",dlt,dlt_name);
	switch(dlt)
	{
	case DLT_EN10MB :
	//case DLT_IEEE802_11:
		break;
	default:
		SysMessage("ERROR: Unsupported DataLink Type (%d): %s",dlt,dlt_name);
		pcap_close(adhandle);
		return -1;
	}

	const std::string plfile(s_strLogPath + "/pkt_log.pcap");
	dump_pcap = pcap_dump_open(adhandle, plfile.c_str());

	pcap_io_running=1;
	emu_printf("Ok.\n");
	return 0;
}

#ifdef _WIN32
int gettimeofday (struct timeval *tv, void* tz)
{
  unsigned __int64 ns100; /*time since 1 Jan 1601 in 100ns units */

  GetSystemTimeAsFileTime((LPFILETIME)&ns100);
  tv->tv_usec = (long) ((ns100 / 10L) % 1000000L);
  tv->tv_sec = (long) ((ns100 - 116444736000000000L) / 10000000L);
  return (0);
} 
#endif

int pcap_io_send(void* packet, int plen)
{
	if(pcap_io_running<=0)
		return -1;

	if(dump_pcap)
	{
		static struct pcap_pkthdr ph;
		gettimeofday(&ph.ts,NULL);
		ph.caplen=plen;
		ph.len=plen;
		pcap_dump((u_char*)dump_pcap,&ph,(u_char*)packet);
	}

	return pcap_sendpacket(adhandle, (u_char*)packet, plen);
}

int pcap_io_recv(void* packet, int max_len)
{
	static struct pcap_pkthdr *header;
	static const u_char *pkt_data1;

	if(pcap_io_running<=0)
		return -1;

	if((pcap_next_ex(adhandle, &header, &pkt_data1)) > 0)
	{
		memcpy(packet,pkt_data1,header->len);

		if(dump_pcap)
			pcap_dump((u_char*)dump_pcap,header,(u_char*)packet);

		return header->len;
	}

	return -1;
}

void pcap_io_close()
{
	if(dump_pcap)
		pcap_dump_close(dump_pcap);
	if (adhandle)
		pcap_close(adhandle);  
	pcap_io_running=0;
}


int pcap_io_get_dev_num()
{
	pcap_if_t *alldevs;
	pcap_if_t *d;
	int i=0;
	
	if(pcap_findalldevs(&alldevs, errbuf) == -1)
	{
		return 0;
	}
    
	d=alldevs;
    while(d!=NULL) {d=d->next; i++;}

	pcap_freealldevs(alldevs);

	return i;
}

char* pcap_io_get_dev_name(int num)
{
	pcap_if_t *alldevs;
	pcap_if_t *d;
	int i=0;

	if(pcap_findalldevs(&alldevs, errbuf) == -1)
	{
		return NULL;
	}
    
	d=alldevs;
    while(d!=NULL) {
		if(num==i)
		{
			strcpy(namebuff,d->name);
			pcap_freealldevs(alldevs);
			return namebuff;
		}
		d=d->next; i++;
	}

	pcap_freealldevs(alldevs);

	return NULL;
}

char* pcap_io_get_dev_desc(int num)
{
	pcap_if_t *alldevs;
	pcap_if_t *d;
	int i=0;

	if(pcap_findalldevs(&alldevs, errbuf) == -1)
	{
		return NULL;
	}
    
	d=alldevs;
    while(d!=NULL) {
		if(num==i)
		{
			strcpy(namebuff,d->description);
			pcap_freealldevs(alldevs);
			return namebuff;
		}
		d=d->next; i++;
	}

	pcap_freealldevs(alldevs);

	return NULL;
}


PCAPAdapter::PCAPAdapter()
{
	//if (config.ethEnable == 0) return; //whut? nada!
	if (pcap_io_init(config.Eth) == -1) {
		SysMessage("Can't open Device '%s'\n", config.Eth);
	}
}
bool PCAPAdapter::blocks()
{
	return false;
}
bool PCAPAdapter::isInitialised()
{
	return !!pcap_io_running;
}
//gets a packet.rv :true success
bool PCAPAdapter::recv(NetPacket* pkt)
{
	int size=pcap_io_recv(pkt->buffer,sizeof(pkt->buffer));
	if(size<=0)
	{
		return false;
	}
	else
	{
		pkt->size=size;
		return true;
	}
}
//sends the packet .rv :true success
bool PCAPAdapter::send(NetPacket* pkt)
{
	if(pcap_io_send(pkt->buffer,pkt->size))
	{
		return false;
	}
	else
	{
		return true;
	}
}
PCAPAdapter::~PCAPAdapter()
{
	pcap_io_close();
}
