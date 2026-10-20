# Spam the Beken ROM link-check (01 E0 FC 01 00) and log every byte received.
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.003)
out = open(sys.argv[2], 'wb'); t0 = time.time(); rx = b''
while time.time() - t0 < float(sys.argv[3]):
    s.write(b'\x01\xe0\xfc\x01\x00')
    d = s.read(256)
    if d:
        out.write(d); out.flush(); rx += d
        if b'\x04\x0e\x05\x01\xe0\xfc\x01\x00' in rx:
            print('LINK OK at %.1fs' % (time.time() - t0)); break
print('rx bytes', len(rx))
