"""Native screenshot of a single test window, including when other desktop windows overlap it."""
import ctypes as C
from ctypes import wintypes as W
from PIL import Image

u = C.WinDLL('user32')
g = C.WinDLL('gdi32')
u.GetWindowRect.argtypes = [W.HWND,C.POINTER(W.RECT)]
u.GetWindowDC.argtypes = [W.HWND]
u.GetWindowDC.restype = W.HDC
u.ReleaseDC.argtypes = [W.HWND,W.HDC]
u.PrintWindow.argtypes = [W.HWND,W.HDC,W.UINT]
g.CreateCompatibleDC.argtypes = [W.HDC]
g.CreateCompatibleDC.restype = W.HDC
g.CreateCompatibleBitmap.argtypes = [W.HDC,C.c_int,C.c_int]
g.CreateCompatibleBitmap.restype = W.HBITMAP
g.SelectObject.argtypes = [W.HDC,W.HGDIOBJ]
g.SelectObject.restype = W.HGDIOBJ
g.DeleteObject.argtypes = [W.HGDIOBJ]
g.DeleteDC.argtypes = [W.HDC]


class Header(C.Structure):
    _fields_ = [('size',W.DWORD),('width',W.LONG),('height',W.LONG),('planes',W.WORD),('bits',W.WORD),
                ('compression',W.DWORD),('imageSize',W.DWORD),('x',W.LONG),('y',W.LONG),
                ('used',W.DWORD),('important',W.DWORD)]


g.GetDIBits.argtypes = [W.HDC,W.HBITMAP,W.UINT,W.UINT,C.c_void_p,C.c_void_p,W.UINT]


def capture(h, path):
    r = W.RECT()
    u.GetWindowRect(h,C.byref(r))
    width,height = r.right-r.left,r.bottom-r.top
    src = u.GetWindowDC(h)
    dc = g.CreateCompatibleDC(src)
    bitmap = g.CreateCompatibleBitmap(src,width,height)
    old = g.SelectObject(dc,bitmap)
    try:
        assert u.PrintWindow(h,dc,2)
        g.SelectObject(dc,old)
        head = Header(C.sizeof(Header),width,-height,1,32,0,0,0,0,0,0)
        data = C.create_string_buffer(width*height*4)
        assert g.GetDIBits(dc,bitmap,0,height,data,C.byref(head),0) == height
        value = Image.frombytes('RGB',(width,height),data.raw,'raw','BGRX')
        value.save(path)
        return value
    finally:
        g.SelectObject(dc,old)
        g.DeleteObject(bitmap)
        g.DeleteDC(dc)
        u.ReleaseDC(h,src)
