# -*- coding: utf-8 -*-
# tcp mapping created by wxk at 2021-12-7

import sys
import socket
import logging
import threading


# 端口映射配置信息
CFG_REMOTE_IP = '127.0.0.1'
CFG_REMOTE_PORT = 22
CFG_LOCAL_IP = '0.0.0.0'
CFG_LOCAL_PORT = 10086

# 接收数据缓存大小
PKT_BUFF_SIZE = 2048

logger = logging.getLogger("Proxy Logging")
formatter = logging.Formatter('%(name)-12s %(asctime)s %(levelname)-8s %(lineno)-4d %(message)s', '%Y %b %d %a %H:%M:%S',)

stream_handler = logging.StreamHandler(sys.stderr)
stream_handler.setFormatter(formatter)
logger.addHandler(stream_handler)

logger.setLevel(logging.DEBUG)

# 单向流数据传递
def tcp_mapping_worker(conn_receiver, conn_sender):
    while True:
        try:
            data = conn_receiver.recv(PKT_BUFF_SIZE)
        except Exception:
            logger.debug('Connection closed.')
            break

        if not data:
            logger.info('No more data is received.')
            break

        try:
            conn_sender.sendall(data)
        except Exception:
            logger.error('Failed sending data.')
            break

        # logger.info('Mapping data > %s ' % repr(data))
        logger.info('Mapping > %s -> %s > %d bytes.' % (conn_receiver.getpeername(), conn_sender.getpeername(), len(data)))

    conn_receiver.close()
    conn_sender.close()

    return

# 端口映射请求处理
def tcp_mapping_request(local_conn, remote_ip, remote_port):
    remote_conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        remote_conn.connect((remote_ip, remote_port))
    except Exception:
        local_conn.close()
        logger.error('Unable to connect to the remote server.')
        return

    # 这种方式是不行的，为什么会被阻塞呢？
    # tcp_mapping_worker(local_conn, remote_conn)
    # tcp_mapping_worker(remote_conn, local_conn)

    threading.Thread(target=tcp_mapping_worker, args=(local_conn, remote_conn)).start()
    threading.Thread(target=tcp_mapping_worker, args=(remote_conn, local_conn)).start()

    return

# 端口映射函数
def tcp_mapping(remote_ip, remote_port, local_ip, local_port):
    local_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    local_server.bind((local_ip, local_port))
    local_server.listen(5)

    logger.debug('Starting mapping service on ' + local_ip + ':' + str(local_port) + ' ...')

    while True:
        try:
            (local_conn, local_addr) = local_server.accept()
        except Exception:
            local_server.close()
            logger.debug('Stop mapping service.')
            break

        threading.Thread(target=tcp_mapping_request, args=(local_conn, remote_ip, remote_port)).start()

        logger.debug('Receive mapping request from %s:%d.' % local_addr)

    return

# 主函数
if __name__ == '__main__':
    tcp_mapping(CFG_REMOTE_IP, CFG_REMOTE_PORT, CFG_LOCAL_IP, CFG_LOCAL_PORT)