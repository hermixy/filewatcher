# filewatcher
This is a standlone daemon, watching the change of folder and file, such as add/del/mod/update ...

Filewatcher can be deployed on CentOS 6.4+ with script

Written based on liblog, libdict and libgevent in libraries

##How To Build

 ** build libraries first
 $ cd ../libraries
 $ ./build.sh
 $ sudo ./build.sh

 ** build filewatcher
 $ cd filewatcher; make
 $ sudo make install

 ** run daemon
 $ sudo cp centos/init.d/filewatcher /etc/init.d/filewatcher
 $ sudo /etc/init.d/filewatcher start
