### myfrp

myfrp is a software for intranet penetration like frp writing in golang. Currently this software only support one frpc connectd to one frps, one frpc service port open and many clients connected to frps at service port.

### usage
**Compile**
Currently it onpy supports linux platform and other platforms will not be supported in the future.

```sh
mkdir build && cd build
cmake ..
make
```
**run**
run frps on your public network machine
```sh
./frps -c ../configs/frps.json
```
run frpc on your internal network machine
```sh
./frpc -c ../configs/frpc.json
```

### todo list
- [] heart beating support
- [] logging support
- [] more than one service port support
- [] more frpc clients connected to frps support


