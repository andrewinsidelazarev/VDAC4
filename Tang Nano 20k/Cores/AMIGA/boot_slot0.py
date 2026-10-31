"""Return the observed FT812 core to existing Tang flash slot 0 via BL616.

Implements core_cache::Client::boot(0); no flash erase/program commands.
For a board whose FT812 image lacks a flash bridge and whose RECONFIG request
does not reload the FPGA. Checks exact FT812 identity before mailbox writes.
"""
import argparse, json, re, socket, time
ap=argparse.ArgumentParser();ap.add_argument('address');ap.add_argument('--source')
args=ap.parse_args()
source=(args.source,0) if args.source else None
with socket.create_connection((args.address,23),timeout=20,source_address=source) as s:
    s.settimeout(.25)
    end=time.monotonic()+1
    while time.monotonic()<end:
        try:s.recv(8192)
        except socket.timeout:pass
    def command(text,pattern):
        s.sendall((text+'\n').encode());buf=bytearray();end=time.monotonic()+20
        while time.monotonic()<end:
            try:
                part=s.recv(8192)
                if not part:break
                buf.extend(part)
            except socket.timeout:pass
            match=re.search(pattern,buf,re.M)
            if match:return match
        raise RuntimeError('No response to '+text+': '+buf.decode('utf8','replace')[-800:])
    def rd(a):
        return int(command(f'tang rd {a:x}',f'^tang {a:02X} = ([0-9A-F]{{8}})'.encode())[1],16)
    def wr(a,v):
        result=command(f'tang wr {a:x} {v:08x}',f'^tang {a:02X} <- [^\r\n]+'.encode())
        print(result[0].decode(),flush=True)
    assert rd(0)==0x56444334
    ver=rd(4)
    if ver>>24!=1:
        assert ver==0x02010129,hex(ver)
        assert rd(0xf0)&0xff00ffff==0xb0000001
        info=rd(0x30);seq=((info&0xffff)+1)&0xffff
        print(json.dumps({'version':hex(ver),'previous_cp_info':hex(info),'boot_slot':0,'sequence':seq}),flush=True)
        wr(0x34,0);wr(0x204,0);wr(0x200,0xb7090000|seq)
        for attempt in range(15):
            time.sleep(1)
            ident=rd(0);version=rd(4)
            print(json.dumps({'id':hex(ident),'version':hex(version)}),flush=True)
            if ident==0x56444334 and version>>24==1:break
            response=rd(0x30)
            if response>>24==0xc2 and response&0xffff==seq and ((response>>16)&255) not in (0,0x10):
                raise RuntimeError('BL616 BOOT rejected: '+hex(response))
        else:raise RuntimeError('TS-Config did not appear after BL616 BOOT')
    print('TS-Config active',flush=True)
