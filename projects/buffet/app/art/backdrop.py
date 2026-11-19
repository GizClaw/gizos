"""Register a painted background to the playable counter. Keep the rear
walkway open and retain independent occlusion pixels for the island."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter


def backdrop(source):
    src=Image.open(source).convert('RGB')
    # Heights are landmarks in the painting and logical floor, respectively.
    # A continuous piecewise-linear warp registers both edges of the rail.
    landmarks=((0,0),(272,132),(355,178),(382,194),(430,225),(478,256),(567,291),(997,467),(1024,480))
    display=Image.new('RGB',(1600,960))
    for (sy,y),(end,dy) in zip(landmarks, landmarks[1:]):
        piece=src.crop((0,sy,1536,end)).resize((1600,(dy-y)*2),Image.Resampling.LANCZOS)
        display.paste(piece,(0,y*2))
    eraser=Image.new('L',(1600,960));mask=ImageDraw.Draw(eraser)
    def point(x,y):return (int(x*2),int(y*2))
    mask.rounded_rectangle([point(151,177),point(647,289)],radius=98,fill=255)
    counter=display.crop((272,344,1328,594)).convert('RGBA')
    counter.putalpha(eraser.crop((272,344,1328,594)).filter(ImageFilter.GaussianBlur(.6)))
    return display,counter
