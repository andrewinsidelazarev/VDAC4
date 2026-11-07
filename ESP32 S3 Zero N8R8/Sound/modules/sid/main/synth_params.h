/* SID voice setup changes on register writes, rather than every PCM sample.
 * Envelopes and all phase/filter integrators continue to advance per sample. */
static INLINE void synth_prepare_params(VoiceCache* cache,const cRSID_SIDinstance* sid,
                                        const uint8_t* regs,const uint8_t* env,int real) {
  const int is8580=sid->ChipModel==8580;
  Voice* v=cache->voice;
  if(!cache->valid||cache->model!=sid->ChipModel||cache->real!=real||
     memcmp(cache->regs,regs,SNAP_REGS)!=0) {
  const uint8_t route=regs[0x17],volband=regs[0x18];
  for (unsigned i = 0; i < 3; ++i) {
    const uint8_t* ch = regs + 7 * i;
    const uint8_t wf = ch[4];
    Voice* x = &v[i];
    x->step = ((uint32_t)ch[1] << 8 | ch[0]) * CYCLES;
    x->test = (wf & 0x08) != 0;
    x->sync = (wf & 0x02) != 0;
    x->ring = (wf & 0x04) != 0;
    x->env = is8580 ? env[i] : cRSID_ADSR_DAC_6581[env[i]];
    x->dest = (route & (1u << i)) ? 2 : (i != 2 || !(volband & 0x80)) ? 1 : 0;
    x->mask = 0xFFF;
    x->table = NULL;
    x->pw = (uint32_t)((ch[3] & 0x0F) << 8 | ch[2]);
    switch (wf & 0xF0) {
      case 0x80: x->kind = WV_NOISE; break;
      case 0x70: x->kind = WV_COMBINED_PULSE; x->table = cRSID_PulseSawTriangle; break;
      case 0x60: x->kind = WV_COMBINED_PULSE; x->table = cRSID_PulseSawtooth; break;
      case 0x50: x->kind = WV_PULTRI; x->table = cRSID_PulseTriangle; break;
      case 0x40: x->kind = WV_PULSE; x->pw <<= 4; break;
      case 0x30: x->kind = WV_COMBINED; x->table = cRSID_SawTriangle; break;
      case 0x20: x->kind = WV_SAW; break;
      case 0x10: x->kind = WV_TRI; break;
      case 0x00:
        x->kind = real && wf == 0x01 ? WV_DIGI : WV_HOLD;   /* SounDemon: частота — ЦАП напрямую */
        x->digi_out = (uint16_t)(ch[1] << 8);
        break;
      default: x->kind = WV_ZERO; break;                     /* шум с другой волной — тишина */
    }
    if (x->table != NULL && !is8580 && x->table != cRSID_PulseTriangle) x->mask = 0x7FF;
  }

    if(is8580) {
      cache->cutoff=cRSID_CutoffMul8580_44100Hz[(regs[0x16]<<3)+(regs[0x15]&7)];
      cache->resonance=cRSID_Resonances8580[regs[0x17]>>4];
    }
    memcpy(cache->regs,regs,SNAP_REGS);
    cache->model=sid->ChipModel;cache->real=(uint8_t)real;cache->valid=1;
  }
  for(unsigned i=0;i<3;++i)v[i].env=is8580?env[i]:cRSID_ADSR_DAC_6581[env[i]];
}

/* The 6581 cutoff depends on signal amplitude and stays in cRSID's model.
 * Only constant 8580 coefficients are cached. Signed divisions, integration
 * order, D418 digi coupling and every stored filter value match cRSID. */
static INLINE int synth_cached_output(cRSID_SIDinstance* sid,const VoiceCache* params) {
  if(sid->ChipModel!=8580||!params->valid)return cRSID_emulateSIDoutputStage(sid);
  const unsigned volume=sid->BasePtr[0x18];
  const int scale=1<<CRSID_FILTERTABLE_RESOLUTION;
  int output=0;
  int t=sid->FilterInputSample+(sid->PrevBandPass*params->resonance)/scale+sid->PrevLowPass;
  if(volume&0x40)output-=t;
  t=sid->PrevBandPass-(t*params->cutoff)/scale;
  sid->PrevBandPass=t;
  if(volume&0x20)output-=t;
  t=sid->PrevLowPass+(t*params->cutoff)/scale;
  sid->PrevLowPass=t;
  if(volume&0x10)output+=t;
  int main_volume;
  if(cRSID_C64.RealSIDmode) {
    t=(volume&15)<<12;
    sid->Digi=(t-sid->PrevVolume)*(16/CRSID_WAVGEN_PREDIV);
    sid->PrevVolume+=(t-sid->PrevVolume)/1024;
    main_volume=sid->PrevVolume>>12;
  } else main_volume=volume&15;
  sid->Output=(sid->NonFiltedSample+output)*main_volume+sid->Digi;
  return sid->Output;
}
