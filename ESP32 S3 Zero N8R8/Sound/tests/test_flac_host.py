"""Real FLAC decoder + PLAYER: PCM, fragmented metadata, tags, covers and seeks."""
import array
import pathlib
import struct
import subprocess
import tempfile
import unittest
import test_player_host as player


def split_flac(data):
    assert data[:4] == b'fLaC'
    blocks = []
    at = 4
    while True:
        kind = data[at]; length = int.from_bytes(data[at+1:at+4], 'big'); at += 4
        blocks.append((kind & 127, data[at:at+length])); at += length
        if kind & 128: return blocks, data[at:]


def pack_flac(blocks, audio):
    return b'fLaC' + b''.join(bytes([t | (128 if i == len(blocks)-1 else 0)]) +
                              len(b).to_bytes(3, 'big') + b for i, (t,b) in enumerate(blocks)) + audio


def comments(*values):
    values = [v.encode('utf-8') for v in values]
    return struct.pack('<II', 0, len(values)) + b''.join(struct.pack('<I', len(v)) + v for v in values)


def picture(kind, rgb, length=3000):
    jpeg = b'\xff\xd8\xff' + bytes(rgb) + bytes(length)
    return (struct.pack('>II', kind, 10) + b'image/jpeg' + struct.pack('>I', 0) +
            struct.pack('>IIIII', 500, 500, 24, 0, len(jpeg)) + jpeg)


class FlacTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = player.build()
        if cls.exe is None or player.FFMPEG is None:
            raise unittest.SkipTest('MSVC and ffmpeg are required')
        cls.temp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_host(self, scenario='mp3', content=None, path=None, **keys):
        keys.setdefault('ext', 'FLA'); keys.setdefault('sized', 1); keys.setdefault('speed', 40)
        args = [str(self.exe), scenario]
        for k,v in keys.items(): args += ['--'+k, str(v)]
        if content is not None:
            path = pathlib.Path(self.temp.name)/'input.flac'; path.write_bytes(content)
        if path is not None: args.append(str(path))
        proc = subprocess.run(args, capture_output=True, timeout=300)
        self.log = proc.stderr.decode('cp866', 'replace')
        output = proc.stdout.decode('cp866', 'replace')
        self.assertEqual(proc.returncode, 0, output+self.log)
        r = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
        for key in ('tasks_after_stop', 'modules_after_stop', 'replies_outside'):
            self.assertEqual(r.get(key, '0'), '0', key)
        return r

    def tone(self, seconds=3, rate=44100, channels=2, bits=16):
        return player.encoded('test.flac', '-ac', str(channels), '-sample_fmt', 's16' if bits==16 else 's32',
                              '-c:a', 'flac', inputs=player.tone(seconds, 997, rate))

    def assert_played(self, r, frames, rate=44100, channels=2):
        self.assertEqual((r['finished'],r['errors'],r['decoded']), ('1','0','1'), self.log)
        self.assertEqual((r['rate'],r['channels'],r['format']), (str(rate),str(channels),'1'))
        self.assertEqual(int(r['pcm_frames']),frames)
        self.assertEqual(r['pcm_discarded'],'0')

    def test_complete_pcm_and_fragmented_headers(self):
        path = self.tone()
        for block in (7, 1024, 8192):
            with self.subTest(block=block):
                r=self.run_host(path=path, block=block, first=1)
                self.assert_played(r,3*44100)
                self.assertIn('(FLAC.ELF)',self.log)
                self.assertIn(r['stream_durations'],('0,3,3','3,3,3'))

    def test_unicode_tags_and_track_number(self):
        blocks,audio=split_flac(self.tone().read_bytes())
        data=pack_flac([blocks[0],(4,comments('TITLE=Бошетунмай','ARTIST=Кино',
                          'ALBUM=Группа крови','DATE=1988-01-01','TRACKNUMBER=06'))], audio)
        r=self.run_host(content=data,block=31)
        self.assert_played(r,3*44100)
        self.assertEqual([r['stream_'+k] for k in ('title','artist','album','year','track')],
                         ['Бошетунмай','Кино','Группа крови','1988','06'])

    def test_large_comments_and_padding_do_not_block(self):
        blocks,audio=split_flac(self.tone().read_bytes())
        data=pack_flac([blocks[0],(4,comments('COMMENT='+'x'*300000,'title=After long comment',
                           'TRACKNUMBER=03')),(1,bytes(2*1024*1024))],audio)
        r=self.run_host(content=data,block=513)
        self.assert_played(r,3*44100)
        self.assertEqual((r['stream_title'],r['stream_track']),('After long comment','03'))

    def test_embedded_front_cover_wins(self):
        blocks,audio=split_flac(self.tone().read_bytes())
        data=pack_flac([blocks[0],(6,picture(4,(0,0,0))),
                       (6,picture(3,(255,255,255),300000)),(6,picture(4,(0,0,0)))],audio)
        r=self.run_host(content=data,**{'cover-wait':1})
        self.assert_played(r,3*44100)
        self.assertEqual((r['cover_flag'],r['thumb_state'],r['thumb_uniform'],r['thumb_byte']),
                         ('1','1','1','255'))

    def test_without_picture_raises_no_cover_flag(self):
        r=self.run_host(path=self.tone(),**{'cover-wait':1})
        self.assertEqual(r['cover_flag'],'0')
        self.assert_played(r,3*44100)

    def test_mono_surround_and_24_bit(self):
        for rate,channels,bits in ((22050,1,16),(48000,2,24),(44100,6,24)):
            with self.subTest(rate=rate,channels=channels,bits=bits):
                r=self.run_host(path=self.tone(2,rate,channels,bits))
                self.assert_played(r,2*rate,rate,min(channels,2))

    def test_pcm_matches_ffmpeg(self):
        for bits in (16,24):
            with self.subTest(bits=bits):
                path=player.encoded('noise.flac','-ac','2','-sample_fmt','s16' if bits==16 else 's32',
                                    '-c:a','flac','-compression_level','8',
                                    inputs=('-f','lavfi','-i','anoisesrc=seed=4271:duration=2:sample_rate=44100'))
                raw=pathlib.Path(self.temp.name)/'pcm.raw'
                r=self.run_host(path=path,raw=raw)
                self.assert_played(r,2*44100)
                proc=subprocess.run([player.FFMPEG,'-v','error','-i',str(path),'-f','f32le','-'],capture_output=True,check=True)
                ref=array.array('f');ref.frombytes(proc.stdout)
                # The existing PCM sink clips and truncates float * 32767 to int16.
                expected=array.array('h',(int(struct.unpack('<f',struct.pack('<f',v*32767))[0]) for v in ref))
                actual=array.array('h');actual.frombytes(raw.read_bytes())
                self.assertEqual(len(actual),len(expected))
                errors=sum(a!=b for a,b in zip(actual,expected))
                self.assertEqual(errors,0,'PCM must match the reference sample for sample')

    def test_seek_restores_metadata_and_resyncs(self):
        path=player.encoded('seek.flac','-ac','2','-sample_fmt','s16','-c:a','flac',
                            inputs=('-f','lavfi','-i','anoisesrc=seed=1988:duration=10:sample_rate=44100'))
        blocks,audio=split_flac(path.read_bytes())
        data=pack_flac([blocks[0],(4,comments('TITLE=Seekable','TRACKNUMBER=07'))],audio)
        r=self.run_host('flush',content=data,cut=max(len(data)*3//10,400*1024),resume=len(data)*6//10+3,base=6000)
        self.assertEqual((r['flush'],r['seconds_after_flush'],r['finished'],r['errors']),('1','6','1','0'))
        self.assertGreater(int(r['pcm_frames'])-int(r['frames_before_flush']),3*44100)
        self.assertEqual((r['stream_title'],r['stream_track']),('Seekable','07'))
        self.assertEqual(r['stream_durations'],'10,10,10')

    def test_256k_ring_starts_without_prefill(self):
        path=player.encoded('prefill.flac','-ac','2','-sample_fmt','s16','-c:a','flac',
                            inputs=('-f','lavfi','-i','anoisesrc=seed=1991:duration=8:sample_rate=44100'))
        self.assertGreater(path.stat().st_size,512*1024)
        r=self.run_host(path=path, first=64*1024)
        self.assert_played(r,8*44100)
        self.assertEqual(r['open_free'],str(256*1024))
        self.assertGreater(int(r['frames_before_rest']),0)
        self.assertEqual(r['open_buffering'],'0')
        self.assertIn('FLAC buffer 256 KB, start 0 KB, refill 96 KB',self.log)
        self.assertEqual(r['free_after_close'],str(128*1024))
        self.assertEqual(r['large_buffers_after_stop'],'0','large ring freed on stop')

    def test_input_gap_does_not_rebuffer_while_pcm_is_ready(self):
        path=player.encoded('input-gap.flac','-ac','2','-sample_fmt','s16','-c:a','flac',
                            inputs=('-f','lavfi','-i','anoisesrc=seed=1988:duration=2:sample_rate=44100'))
        r=self.run_host(path=path,first=96*1024,speed=1,**{'pcm-ms':1500})
        self.assert_played(r,2*44100)
        self.assertEqual(r['input_before_rest'],'0','decoder has used all incoming bytes')
        self.assertGreater(int(r['pcm_ms_before_rest']),100,'PCM still covers the input gap')
        self.assertEqual(r['buffering_before_rest'],'0','keep accepting frames without a refill pause')
        self.assertEqual(r['underruns_before_rest'],'0')

    def test_small_file_finishes_below_the_prefill_threshold(self):
        path=self.tone(1)
        self.assertLess(path.stat().st_size,384*1024)
        r=self.run_host(path=path)
        self.assert_played(r,44100)

    def test_low_memory_keeps_usable_watermarks(self):
        r=self.run_host(path=self.tone(2),psram=350000)
        self.assert_played(r,2*44100)
        self.assertEqual(r['open_free'],str(128*1024))
        self.assertIn('FLAC buffer 128 KB, start 0 KB, refill 48 KB',self.log)

    def test_missing_decoder(self):
        r=self.run_host('missing')
        self.assertEqual(r['open'],'0')
        self.assertIn('FLAC.ELF not loaded',self.log)

    def test_invalid_and_truncated_metadata(self):
        for data in (b'not FLAC',b'fLaC\x80\x00\x00\x22'+bytes(5),b'fLaC\xff\x00\x00\x00'):
            with self.subTest(data=data):
                r=self.run_host(content=data)
                self.assertEqual((r['finished'],r['pcm_frames']),('1','0'))
                self.assertGreater(int(r['errors']),0)


if __name__=='__main__': unittest.main()
