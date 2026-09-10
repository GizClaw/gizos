#include "h2_lua_vector_cg.h"
#include "h2_lua_vector_delta.h"
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <string.h>

typedef struct { const uint8_t *p, *end; int valid; } reader_t;
static unsigned byte(reader_t *r) {if(r->p==r->end){r->valid=0;return 0;}return *r->p++;}
static unsigned word(reader_t *r) {unsigned a=byte(r);return a|(byte(r)<<8);}
static double number(reader_t *r) {
  uint32_t bits=byte(r);bits|=byte(r)<<8;bits|=byte(r)<<16;bits|=byte(r)<<24;
  float v;memcpy(&v,&bits,4);
  if(!isfinite(v)||fabs(v)>1000000){r->valid=0;return 0;}return v;
}
typedef struct { CGGradientRef value; unsigned radial; double xy[4]; } gradient_t;
int h2_lua_vector_cg_render(const uint8_t *data,size_t length,uint8_t *rgba,
                          unsigned width,unsigned height,const double matrix[6]) {
  if(!data||length<12||length>2000000||memcmp(data,"H2VG",4)||!rgba||
     !width||!height||width>4096||height>4096)return 0;
  reader_t r={data+4,data+length,1};unsigned vw=word(&r),vh=word(&r),count=word(&r),version=word(&r);
  if(!vw||!vh||vw>4096||vh>4096||count>16||(version!=1&&version!=2))return 0;
  gradient_t gradients[16]={0};CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx=NULL;CGMutablePathRef path=NULL;int depth=0,success=0;
  if(!space)return 0;
  for(unsigned i=0;i<count&&r.valid;i++) {
    gradient_t *g=&gradients[i];g->radial=byte(&r);unsigned stops=byte(&r);
    for(unsigned j=0;j<4;j++)g->xy[j]=number(&r);
    if(g->radial>1||!stops||stops>16||(g->radial&&g->xy[2]<=0))goto done;
    CGFloat locations[16],components[64];double previous=-1;
    for(unsigned j=0;j<stops;j++) {
      locations[j]=number(&r);
      if(locations[j]<0||locations[j]>1||locations[j]<previous)goto done;
      previous=locations[j];
      for(unsigned k=0;k<4;k++)components[j*4+k]=byte(&r)/255.;
    }
    if(!r.valid)goto done;
    g->value=CGGradientCreateWithColorComponents(space,components,locations,stops);
    if(!g->value)goto done;
  }
  memset(rgba,0,(size_t)width*height*4);
  ctx=CGBitmapContextCreate(rgba,width,height,8,(size_t)width*4,space,
                           kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
  if(!ctx)goto done;
  CGContextTranslateCTM(ctx,0,height);CGContextScaleCTM(ctx,1,-1);
  CGContextConcatCTM(ctx,CGAffineTransformMake(matrix[0],matrix[1],matrix[2],matrix[3],matrix[4],matrix[5]));
  CGContextSetLineCap(ctx,kCGLineCapRound);CGContextSetLineJoin(ctx,kCGLineJoinRound);
  unsigned commands=0;
  while(r.valid&&r.p<r.end&&++commands<65536) {
    unsigned opcode=byte(&r);double v[6];
    switch(opcode) {
    case 0:success=r.valid&&depth==0&&r.p==r.end;goto done;
    case 1:if(depth>=32)goto done;depth++;CGContextSaveGState(ctx);break;
    case 2:if(depth<=0)goto done;--depth;CGContextRestoreGState(ctx);break;
    case 3:
      for(unsigned j=0;j<6;j++)v[j]=number(&r);
      if(!r.valid||fabs(v[0]*v[3]-v[1]*v[2])<1e-12)goto done;
      CGContextConcatCTM(ctx,CGAffineTransformMake(v[0],v[1],v[2],v[3],v[4],v[5]));break;
    case 4:if(path)CGPathRelease(path);path=CGPathCreateMutable();if(!path)goto done;break;
    case 5:case 6:
      v[0]=number(&r);v[1]=number(&r);if(!path||!r.valid)goto done;
      if(opcode==5)CGPathMoveToPoint(path,NULL,v[0],v[1]);else CGPathAddLineToPoint(path,NULL,v[0],v[1]);break;
    case 7:
      for(unsigned j=0;j<6;j++)v[j]=number(&r);if(!path||!r.valid)goto done;
      CGPathAddCurveToPoint(path,NULL,v[0],v[1],v[2],v[3],v[4],v[5]);break;
    case 8:if(!path)goto done;CGPathCloseSubpath(path);break;
    case 13: {
      /* Compact polygon, signed quarter-unit coordinates. Still geometry;
       * vertices participate in the same complexity bound as float paths. */
      unsigned n=word(&r);if(!path||n<3||commands+n>=65536)goto done;
      commands+=n;
      for(unsigned j=0;j<n;j++) {
        double x=(int16_t)word(&r)/4.,y=(int16_t)word(&r)/4.;
        if(!r.valid)goto done;
        if(!j)CGPathMoveToPoint(path,NULL,x,y);else CGPathAddLineToPoint(path,NULL,x,y);
      }
      CGPathCloseSubpath(path);break;
    }
    case 14: {
      unsigned n=word(&r),unit=byte(&r);
      if(version!=2||!path||n<3||commands+n>=65536||(unit!=1&&unit!=4))goto done;
      commands+=n;int32_t x=0,y=0;
      for(unsigned j=0;j<n;j++) {
        if(!r.valid||!h2_lua_vector_delta_next(&r.p,r.end,unit,&x)||
           !h2_lua_vector_delta_next(&r.p,r.end,unit,&y))goto done;
        if(!j)CGPathMoveToPoint(path,NULL,x/4.,y/4.);
        else CGPathAddLineToPoint(path,NULL,x/4.,y/4.);
      }
      CGPathCloseSubpath(path);break;
    }
    case 9:
      for(unsigned j=0;j<4;j++)v[j]=number(&r);if(!path||!r.valid||v[2]<=0||v[3]<=0)goto done;
      CGPathAddEllipseInRect(path,NULL,CGRectMake(v[0],v[1],v[2],v[3]));break;
    case 10: {
      unsigned stroke=byte(&r);int brush=(int16_t)word(&r);double color[4];
      for(unsigned j=0;j<4;j++)color[j]=byte(&r)/255.;
      double line_width=number(&r),opacity=number(&r);
      if(!path||!r.valid||stroke>1||brush< -1||brush>=(int)count||opacity<0||opacity>1||line_width<0)goto done;
      CGContextSaveGState(ctx);CGContextSetAlpha(ctx,opacity);CGContextSetLineWidth(ctx,line_width);
      CGContextAddPath(ctx,path);
      if(brush<0) {
        if(stroke){CGContextSetRGBStrokeColor(ctx,color[0],color[1],color[2],color[3]);CGContextStrokePath(ctx);}
        else {CGContextSetRGBFillColor(ctx,color[0],color[1],color[2],color[3]);CGContextFillPath(ctx);}
      } else {
        CGRect bounds=CGPathGetPathBoundingBox(path);gradient_t *g=&gradients[brush];
        if(stroke)CGContextReplacePathWithStrokedPath(ctx);
        CGContextClip(ctx);
        if(bounds.size.width>0&&bounds.size.height>0) {
          CGContextTranslateCTM(ctx,bounds.origin.x,bounds.origin.y);
          CGContextScaleCTM(ctx,bounds.size.width,bounds.size.height);
          if(g->radial)CGContextDrawRadialGradient(ctx,g->value,CGPointMake(g->xy[0],g->xy[1]),0,
            CGPointMake(g->xy[0],g->xy[1]),g->xy[2],kCGGradientDrawsBeforeStartLocation|kCGGradientDrawsAfterEndLocation);
          else CGContextDrawLinearGradient(ctx,g->value,CGPointMake(g->xy[0],g->xy[1]),CGPointMake(g->xy[2],g->xy[3]),
            kCGGradientDrawsBeforeStartLocation|kCGGradientDrawsAfterEndLocation);
        }
      }
      CGContextRestoreGState(ctx);break;
    }
    case 11:if(!path)goto done;CGContextAddPath(ctx,path);CGContextClip(ctx);break;
    default:goto done;
    }
  }
done:
  if(path)CGPathRelease(path);
  if(ctx){while(depth-->0)CGContextRestoreGState(ctx);CGContextRelease(ctx);}
  for(unsigned i=0;i<count;i++)if(gradients[i].value)CGGradientRelease(gradients[i].value);
  CGColorSpaceRelease(space);return success;
}
