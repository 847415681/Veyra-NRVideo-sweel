"""Same-source coded-luma reconstruction error, box-window SSIM and crop evidence."""
from pathlib import Path
import hashlib,json,math,sys
import numpy as np
from PIL import Image,ImageDraw

def measure(reference,candidate):
    a=np.asarray(reference.convert('RGB'),dtype=np.float32);b=np.asarray(candidate.convert('RGB'),dtype=np.float32)
    assert a.shape==b.shape
    a=a@np.array([.2126,.7152,.0722],np.float32);b=b@np.array([.2126,.7152,.0722],np.float32)
    mse=float(np.mean((a-b)**2));edge=float((np.mean(abs(np.diff(a,axis=0)-np.diff(b,axis=0)))+np.mean(abs(np.diff(a,axis=1)-np.diff(b,axis=1))))/2)
    h=a.shape[0]//8*8;w=a.shape[1]//8*8
    aa=a[:h,:w].reshape(h//8,8,w//8,8).transpose(0,2,1,3);bb=b[:h,:w].reshape(h//8,8,w//8,8).transpose(0,2,1,3)
    ma=aa.mean((2,3));mb=bb.mean((2,3));va=((aa-ma[:,:,None,None])**2).mean((2,3));vb=((bb-mb[:,:,None,None])**2).mean((2,3))
    cov=((aa-ma[:,:,None,None])*(bb-mb[:,:,None,None])).mean((2,3))
    ssim=((2*ma*mb+6.5025)*(2*cov+58.5225))/((ma**2+mb**2+6.5025)*(va+vb+58.5225))
    return {'psnrY':10*math.log10(255**2/mse) if mse else 100.0,'maeY':float(np.mean(abs(a-b))),'mseY':mse,'ssim8x8':float(ssim.mean()),'gradientMaeY':edge}

def main():
    if sys.argv[1]=='compare':
        compare(Path(sys.argv[2]),Path(sys.argv[3]),Path(sys.argv[4]));return
    folder=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    refs=sorted(folder.glob('reference*.png')) or [folder/'4-native-off.png'];rows=[]
    for refPath in refs:
        ref=Image.open(refPath).convert('RGB');suffix=refPath.stem.removeprefix('reference')
        candidates=sorted(folder.glob('*'+suffix+'.png')) if refPath.stem.startswith('reference') else sorted(folder.glob('*upscale-on.png'))
        for file in candidates:
            if file==refPath:continue
            candidate=Image.open(file).convert('RGB')
            row={'reference':refPath.name,'candidate':file.name,**measure(ref,candidate)};rows.append(row)
            print(json.dumps(row,ensure_ascii=False),flush=True)
        if not candidates:continue
        # Fixed positions are recorded and identical for all algorithms. No rescaling.
        boxes=[(ref.width//4,ref.height//3,ref.width//4+384,ref.height//3+256),(ref.width//2,ref.height//2,ref.width//2+384,ref.height//2+256)]
        chosen=[refPath]+[p for p in candidates if p!=refPath and 'color' not in p.name]
        for index,box in enumerate(boxes):
            sheet=Image.new('RGB',(384*len(chosen),284),(25,25,25));draw=ImageDraw.Draw(sheet)
            for i,file in enumerate(chosen):
                with Image.open(file) as im:sheet.paste(im.convert('RGB').crop(box),(384*i,28))
                draw.text((384*i+8,8),file.stem,fill='white')
            sheet.save(out/('crops'+suffix+'-'+str(index)+'.png'))
    report={'method':'BT709 coded luma; non-overlapping 8x8 population SSIM (not Gaussian SSIM); same source/extent; no quality-percentage inference','rows':rows}
    (out/'metrics.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')

def compare(baseline,candidate,out):
    out.mkdir(parents=True,exist_ok=True);rows=[];regions={}
    for refPath in sorted(baseline.glob('reference*.png')):
        counterpart=candidate/refPath.name
        assert hashlib.sha256(refPath.read_bytes()).digest()==hashlib.sha256(counterpart.read_bytes()).digest(),refPath.name
        ref=Image.open(refPath).convert('RGB');suffix=refPath.stem.removeprefix('reference')
        for oldPath in sorted(baseline.glob('*'+suffix+'.png')):
            if oldPath==refPath:continue
            newPath=candidate/oldPath.name
            old=Image.open(oldPath).convert('RGB');new=Image.open(newPath).convert('RGB')
            delta=np.asarray(old,dtype=np.int16)-np.asarray(new,dtype=np.int16)
            row={'frame':refPath.name,'backend':oldPath.name,'before':measure(ref,old),'after':measure(ref,new),
                 'changedPixels':int(np.count_nonzero(np.any(delta,axis=2))),'oldNewMaeRgb':float(np.abs(delta).mean())}
            rows.append(row)
            if suffix!='-f32':continue
            # Pick two separated texture regions using ONLY the reference.
            # Every backend uses the same boxes, independent of its output.
            if suffix not in regions:
                y=np.asarray(ref,dtype=np.float32)@np.array([.2126,.7152,.0722],np.float32)
                tiles=[]
                for fy in (.25,.375,.5,.625,.75):
                    for fx in (.25,.375,.5,.625,.75):
                        x0=int(ref.width*fx)-192;y0=int(ref.height*fy)-128
                        tile=y[y0:y0+256,x0:x0+384]
                        score=float(np.abs(np.diff(tile,axis=0)).mean()+np.abs(np.diff(tile,axis=1)).mean())
                        tiles.append((score,(x0,y0,x0+384,y0+256)))
                boxes=[]
                for _,box in sorted(tiles,reverse=True):
                    if all(abs(box[0]-other[0])+abs(box[1]-other[1])>=600 for other in boxes):boxes.append(box)
                    if len(boxes)==2:break
                regions[suffix]=boxes
            if oldPath.name.startswith('bilinear'):continue
            for i,box in enumerate(regions[suffix]):
                sheet=Image.new('RGB',(1152,284),(25,25,25));draw=ImageDraw.Draw(sheet)
                for col,(label,im) in enumerate((('4K reference',ref),('before '+oldPath.stem,old),('after '+oldPath.stem,new))):
                    sheet.paste(im.crop(box),(384*col,28));draw.text((384*col+8,8),label,fill='white')
                sheet.save(out/(oldPath.stem+'-'+str(i)+'.png'))
            print(json.dumps(row,ensure_ascii=False),flush=True)
    (out/'comparison.json').write_text(json.dumps({'baseline':str(baseline),'candidate':str(candidate),
        'referenceIdentical':True,'regions':regions,'method':'same method as measure(); regions selected only from reference gradient; metrics are not perceptual quality percentages','rows':rows},ensure_ascii=False,indent=2),encoding='utf8')
if __name__=='__main__':main()
