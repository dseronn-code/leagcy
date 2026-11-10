import csv,sys
from PIL import Image, ImageDraw
rows=list(csv.DictReader(open(sys.argv[1]))); out=sys.argv[2]; idx=[int(x) for x in sys.argv[3].split(',')]
S=4; W=int(110*S); H=int(72*S)
def P(x,z): return (W/2+x*S, H/2+z*S)
frames=[]
for i in idx:
  r=rows[i]; im=Image.new('RGB',(W,H+16),(40,110,50)); d=ImageDraw.Draw(im)
  d.rectangle([P(-52.5,-34),P(52.5,34)],outline=(255,255,255)); d.line([P(0,-34),P(0,34)],fill=(255,255,255))
  for gx in (-52.5,52.5): d.rectangle([P(gx-1 if gx>0 else gx-1,-3.66),P(gx+1,3.66)],outline=(255,255,0))
  for p in range(22):
    x,z=float(r[f'p{p}x']),float(r[f'p{p}z']); c=(220,40,40) if p<11 else (60,120,255)
    if p in (0,11): c=(255,160,0) if p==0 else (120,220,255)
    q=P(x,z); d.ellipse([q[0]-4,q[1]-4,q[0]+4,q[1]+4],fill=c, outline=(255,255,255) if int(r['owner'])==p else None)
  bx,bz=float(r['bx']),float(r['bz']); q=P(bx,bz); d.ellipse([q[0]-3,q[1]-3,q[0]+3,q[1]+3],fill=(255,255,255))
  d.text((4,H+2),f"t={int(r['tick'])/30:.1f}s fase={r['phase']} dono={r['owner']} {r['score0']}x{r['score1']} bola y={float(r['by']):.1f}",fill=(255,255,255))
  frames.append(im)
cols=3; rows_=(len(frames)+cols-1)//cols
sheet=Image.new('RGB',(W*cols,(H+16)*rows_))
for k,f in enumerate(frames): sheet.paste(f,((k%cols)*W,(k//cols)*(H+16)))
sheet.save(out)
