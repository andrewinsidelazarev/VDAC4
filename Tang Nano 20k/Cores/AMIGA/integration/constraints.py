"""Board requirements shared by generated Amiga projects."""
import re

def tmds_drive4(text):
    for port in ['O_tmds_clk_p',*[f'O_tmds_data_p[{i}]' for i in range(3)]]:
        pattern=r'(IO_PORT\s+"'+re.escape(port)+r'"\s+)([^;]*);'
        def replace(match):
            attrs=match[2]
            if re.search(r'\bDRIVE\s*=',attrs):attrs=re.sub(r'\bDRIVE\s*=\s*\d+\b','DRIVE=4',attrs)
            else:attrs=attrs.rstrip()+' DRIVE=4'
            return match[1]+attrs+';'
        text,count=re.subn(pattern,replace,text)
        if count!=1:raise ValueError('Expected one TMDS constraint: '+port)
    return text
