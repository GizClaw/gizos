"""Register one rounded visitor to a 64 by 72 projected cell, sole at 32,65."""
from PIL import Image, ImageOps


class Visitor:
    def __init__(self, directory):
        self.poses = {p.stem: Image.open(p).convert('RGBA') for p in directory.glob('*.png')}
        if any(image.size != (256, 288) for image in self.poses.values()):
            raise ValueError('Player poses must share the 4x sole registration.')

    def frame(self, face, pose):
        home = ('front', 'right', 'back', 'right')[face]
        step = ('walk-front', 'walk-right', 'walk-back', 'walk-right')[face]
        key = home
        if pose in (1, 3): key = step
        if pose in (2, 4) and face in (1, 3): key = 'side'
        if pose in (5, 6, 7): key = ('reach', 'bite', 'savor')[pose-5]
        if pose == 8: key = home
        image = self.poses[key]
        if pose in (1, 2, 3, 4):
            phase = (-1, -.25, 1, .25)[pose-1]
            def inverse(x,y):
                height = max(0, min(1, (y-43)/217))
                leg = max(0,(height-.64)/.36)
                side = max(-1,min(1,(x-128)/22))
                locks = max(0,1-abs(height-.48)/.26)*max(0,(abs(x-128)-34)/42)
                dx = phase*(3.5*locks+(16 if face in (1,3) else 3)*leg*side)
                dy = (1-abs(phase))*3*(1-height)+max(0,-phase*side)*leg*9
                return x-dx, y+dy
            mesh=[]
            for y in range(0,288,16):
                for x in range(0,256,16):
                    corners=[inverse(x,y),inverse(x,y+16),inverse(x+16,y+16),inverse(x+16,y)]
                    mesh.append(((x,y,x+16,y+16),tuple(v for pt in corners for v in pt)))
            image=image.transform(image.size,Image.Transform.MESH,mesh,Image.Resampling.BICUBIC)
        return ImageOps.mirror(image) if face==3 else image
