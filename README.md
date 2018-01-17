Dynamic task-management between interconnected L4Re instances  
====================================  
  
This project implements a network task-manager for L4Re (version 2014053111; a snapshot is included in the repository). L4Re is a real-time capable microkernel OS; more info can be found at https://l4re.org/. The OS, however, so far only supported static configurations, meaning all the tasks that should run on the system would have to be known before boot-up (actually even before image creation). This project enables the dynamic starting, stopping and transferring of tasks over the network during runtime.  
  
The core, dom0 (found inside 'dom0-main'), can communicate with other dom0-instances running on different machines. Under the hood, it uses a modified version of Ned, and thus can send and understand all LUA-commands supported by Ned, e.g. for task creation and termination. In addition, it can send and receive binaries to and from other dom0-instances, in order to execute programs that are not present on the file system of the target machine.  
  
In order to work, dom0 needs a modified version of l4re_kernel and Ned, found inside the directories 'dom0-l4re_kernel' and 'dom0-ned'. The differences to the original programs can be seen in the corresponding patchfiles inside 'l4re-modifications'. 
Furthermore, this directory contains the neccessary configuration to run a test scenario. The test scenario demonstrates all of the above mentioned features.  
  



Compile and run test scenario:  
-------------------------  
#dependencies (for debian)  
sudo apt-get install make gawk g++ binutils pkg-config g++-multilib subversion dialog qemu  
make -C l4re-snapshot-2014053111 setup  
#select x86-32  
make -C l4re-snapshot-2014053111/obj/fiasco/ia32 -j4  
make -C l4re-snapshot-2014053111/obj/l4/x86 -j4  
cp l4re-modifications/run.sh l4re-snapshot-2014053111/obj/l4/x86  
cat l4re-modifications/Makeconf.boot.add >> l4re-snapshot-2014053111/obj/l4/x86/conf/Makeconf.boot  
make -C dom0-main/ O=../l4re-snapshot-2014053111/obj/l4/x86/  
make -C dom0-ned/ O=../l4re-snapshot-2014053111/obj/l4/x86/  
make -C dom0-l4re_kernel/ O=../l4re-snapshot-2014053111/obj/l4/x86/  
cd l4re-snapshot-2014053111/obj/l4/x86  
./run.sh  
