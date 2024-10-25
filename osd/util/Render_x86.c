#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Display* display=NULL;
Window window =NULL;
cairo_surface_t* surface=NULL;
cairo_surface_t* image_surface = NULL;
cairo_t* cr=NULL;
int Init_x86(uint16_t *width, uint16_t *height) {
        display = XOpenDisplay(NULL);
        if (!display) {
            fprintf(stderr, "Cannot open display\n");
            return 1;
        }

        int screen = DefaultScreen(display);
        Window root = RootWindow(display, screen);


        // Get the screen width and height
        uint16_t screen_width = DisplayWidth(display, screen);
        uint16_t screen_height = DisplayHeight(display, screen);
        *width=screen_width;
        *height=screen_height;
        // Set up an ARGB visual for transparency
        XVisualInfo vinfo;
        if (!XMatchVisualInfo(display, screen, 32, TrueColor, &vinfo)) {
            fprintf(stderr, "No 32-bit visual found\n");
            return 1;
        }

        XSetWindowAttributes attrs;
        attrs.colormap = XCreateColormap(display, root, vinfo.visual, AllocNone);
        attrs.border_pixel = 0;
        attrs.background_pixel = 0;
//!!!!!!!!!!!!!!!!!!!!!!!!        
        attrs.override_redirect = False;  // Make the window borderless, MAKE TRUE
        

        // Create the window with transparency support
        window = XCreateWindow(display, root,
                                    0, 0, *width, *height, 0, 
                                    vinfo.depth, InputOutput, 
                                    vinfo.visual, 
                                    CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect, 
                                    &attrs);

        // Make the window visible
        XMapWindow(display, window);

        // Set window properties to make it transparent
        if (false){
            Atom windowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
            Atom windowTypeDock = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
            XChangeProperty(display, window, windowType, XA_ATOM, 32, PropModeReplace, (unsigned char*)&windowTypeDock, 1);
        }
        // Create a Cairo surface for drawing on the X11 window
        surface = cairo_xlib_surface_create(display, window, vinfo.visual, *width, *height);
        cr = cairo_create(surface);

}

void premultiplyAlpha(uint32_t* rgbaData, uint32_t width, uint32_t height) {
    for (uint32_t i = 0; i < width * height; ++i) {
        uint8_t* pixel = (uint8_t*)&rgbaData[i];
        uint8_t a = pixel[3]; // Alpha channel

        // Premultiply RGB by alpha
        pixel[0] = (pixel[0] * a) / 255; // Blue channel
        pixel[1] = (pixel[1] * a) / 255; // Green channel
        pixel[2] = (pixel[2] * a) / 255; // Red channel
    }
}

    //Render_x86(rgbaData,bmpBuff.u32Width, bmpBuff.u32Height);
void Render_x86( unsigned char* rgbaData, int u32Width, int u32Height){   

    premultiplyAlpha((uint32_t*)rgbaData, u32Width, u32Height);
    // Create a Cairo image surface from the RGBA data
    if (image_surface==NULL){
        image_surface = cairo_image_surface_create_for_data(
        rgbaData, CAIRO_FORMAT_ARGB32, u32Width, u32Height, u32Width * 4);
    }else{
         // Update the image data if necessary (e.g., if rgbaData changes)
        premultiplyAlpha((uint32_t*)rgbaData, u32Width, u32Height);
       // Update the Cairo surface data if needed (skip if the data is static)
        cairo_surface_mark_dirty(image_surface);
    }

    // Clear the window with transparency
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE );
    cairo_paint(cr);

    // Draw the image onto the Cairo surface at position (1, 1)
    cairo_set_source_surface(cr, image_surface, 1, 1);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_paint(cr);

    // Flush drawing to the window
    cairo_surface_flush(surface);
    XFlush(display);

     

}
void Close_x86(){
    // Clean up resources
    cairo_destroy(cr);
    cairo_surface_destroy(image_surface);
    cairo_surface_destroy(surface);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}

