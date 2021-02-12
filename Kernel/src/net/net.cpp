#include <net/net.h>

#include <net/networkadapter.h>
#include <net/8254x.h>
#include <net/socket.h>

#include <endian.h>
#include <logging.h>
#include <errno.h>
#include <hash.h>

namespace Network {
    NetFS netFS;

    lock_t adaptersLock = 0;
    Vector<NetworkAdapter*> adapters;

    HashMap<uint32_t, MACAddress> addressCache;

    void InitializeDrivers(){
	    Intel8254x::DetectAndInitialize();
    }

    void InitializeConnections(){
        FindMainAdapter();

        if(!mainAdapter) {
            Log::Info("No network adapter found!");
            return;
        }

        InitializeNetworkThread();
    }

    int IPLookup(NetworkAdapter* adapter, const IPv4Address& ip, MACAddress& mac){
        if(addressCache.get(ip.value, mac)){
            return 0;
        }

        uint8_t buffer[sizeof(EthernetFrame) + sizeof(ARPHeader)];
        
		EthernetFrame* ethFrame = reinterpret_cast<EthernetFrame*>(buffer);
		ethFrame->etherType = EtherTypeARP;
		ethFrame->src = adapter->mac;
		ethFrame->dest = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

        ARPHeader* arp = reinterpret_cast<ARPHeader*>(buffer + sizeof(EthernetFrame));
        arp->destHwAddr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        arp->srcHwAddr = adapter->mac;
        arp->hLength = 6;
        arp->hwType = 1; // Ethernet

        arp->destPrAddr = ip.value;
        arp->srcPrAddr = adapter->adapterIP.value;
        arp->pLength = 4;
        arp->prType = EtherTypeARP;

        arp->opcode = ARPHeader::ARPRequest;

        Network::Send(buffer, sizeof(EthernetFrame) + sizeof(ARPHeader), adapter);

        int timer = 1000;
        while(!addressCache.find(ip.value) && timer > 0){
            timer--;
            Timer::Wait(1);
        }

        if(timer <= 0){
            IF_DEBUG((debugLevelNetwork >= DebugLevelNormal), {
                Log::Warning("[Network] [ARP] Timed out waiting for ARP reply.");
            });
            return -EADDRNOTAVAIL;
        }

        addressCache.get(ip.value, mac);

        return 0;
    }

    int Route(const IPv4Address& local, const IPv4Address& dest, MACAddress& mac, NetworkAdapter*& adapter){
        bool isLocalDestination = false;
        IPv4Address localDestination;

        if(adapter){
            if(local.value != INADDR_ANY && local.value != adapter->adapterIP.value){ // If the socket address is not 0 then make sure it aligns with the adapter
                return -ENETUNREACH;
            }

            // Check if the destination is within the subnet
            if((dest.value & adapter->subnetMask.value) == (adapter->adapterIP.value & adapter->subnetMask.value)){
                isLocalDestination = true;
                localDestination = dest;
            } else {
                localDestination = adapter->gatewayIP; // Destination is to WAN, 
            }
        } else {
            for(NetworkAdapter* a : adapters){
                if(local.value != INADDR_ANY && a->adapterIP.value != local.value){
                    continue; // Local address does not correspond to the adapter IP address
                }

                // Check if the destination is within the subnet
                if((dest.value & a->subnetMask.value) == (a->adapterIP.value & a->subnetMask.value)){
                    isLocalDestination = true;
                    adapter = a;
                    localDestination = dest; // Destination is already local
                } else if(!isLocalDestination && a->gatewayIP.value > 0){
                    adapter = a; // If a local route has not been found and the adapter has been assigned a gateway, pick this adapter
                    localDestination = a->gatewayIP;
                }
            }
        }

        if(!adapter){
            Log::Warning("[Network] Could not find any adapters!");
            return -ENETUNREACH;
        }

        if(isLocalDestination){
            int status = IPLookup(adapter, dest, mac);
            if(status < 0){
                return status; // Error obtaining MAC address for IP
            }
        } else {
            int status = IPLookup(adapter, adapter->gatewayIP, mac);
            if(status < 0){
                return status; // Error obtaining MAC address for IP
            }
        }
        return 0;
    }

    NetFS* NetFS::instance = nullptr;
    NetFS::NetFS() : Device("net", DeviceTypeNetworkStack){
        if(instance){
            return; // Instance already exists!
        }

        instance = this;
        flags = FS_NODE_DIRECTORY;
    }

    int NetFS::ReadDir(DirectoryEntry* dirent, uint32_t index){
        if(index == 0){
            strcpy(dirent->name, ".");

            dirent->flags = FS_NODE_DIRECTORY;
            return 1;
        } else if(index == 1){
            strcpy(dirent->name, "..");

            dirent->flags = FS_NODE_DIRECTORY;
            return 1;
        }

        if(index >= adapters.get_length() + 2){
            return 0; // Out of range
        }

        NetworkAdapter* adapter = adapters[index - 2];
        strcpy(dirent->name, adapter->name);

        dirent->flags = FS_NODE_CHARDEVICE;

        return 1;
    }

    FsNode* NetFS::FindDir(char* name){
        if(strcmp(name, ".") == 0){
            return this;
        } else if(strcmp(name, "..") == 0){
            return DeviceManager::GetDevFS();
        }

        for(NetworkAdapter* adapter : adapters){
            if(strcmp(name, adapter->name) == 0){
                return adapter;
            }
        }

        return nullptr;
    }

    void NetFS::RegisterAdapter(NetworkAdapter* adapter){
        acquireLock(&adaptersLock);

        adapter->adapterIndex = adapters.get_length();
        adapters.add_back(adapter);

        releaseLock(&adaptersLock);
    }

    NetworkAdapter* NetFS::FindAdapter(const char* name, size_t len){
        for(NetworkAdapter* adapter : adapters){
            if(strncmp(name, adapter->name, len) == 0){
                return adapter;
            }
        }

        return nullptr;
    }

    NetworkAdapter* NetFS::FindAdapter(uint32_t ip){
        for(NetworkAdapter* adapter : adapters){
            if(adapter->adapterIP.value == ip){
                return adapter; // Found adapter with IP address
            }
        }

        return nullptr;
    }
}