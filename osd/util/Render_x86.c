#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../bmp/bitmap.h"

Display* display=NULL;
Window window =NULL;
cairo_surface_t* surface=NULL;
cairo_surface_t* image_surface = NULL;
cairo_t* cr=NULL;
Pixmap backbuffer_pixmap;
cairo_surface_t* backbuffer_surface;

int Init_x86(uint16_t *width, uint16_t *height) {
        display = XOpenDisplay(NULL);
        if (!display) {
            fprintf(stderr, "Cannot open display\n");
            return 1;
        }
        int screen = DefaultScreen(display);
        Window root = RootWindow(display, screen);
        
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
        attrs.override_redirect = True;  // Make the window borderless, MAKE TRUE
        

        // Create the window with transparency support
        window = XCreateWindow(display, root,
                                    0, 0, *width, *height, 0, 
                                    vinfo.depth, InputOutput, 
                                    vinfo.visual, 
                                    CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect,                                     
                                    &attrs);

        char buffer[50];
        snprintf(buffer, sizeof(buffer), "%lu", window);
        setenv("MSP_WINDOW_ID", buffer, 1);  // 1 means overwrite if it already exists

        // Make the window visible
        XMapWindow(display, window);

        // Raise the window to the top
        XRaiseWindow(display, window);

        // Set input focus to the window
        //XSetInputFocus(display, window, RevertToParent, CurrentTime);

        // Set window properties to make it transparent
        if (false){
            Atom windowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
            Atom windowTypeDock = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
            XChangeProperty(display, window, windowType, XA_ATOM, 32, PropModeReplace, (unsigned char*)&windowTypeDock, 1);
        }
        // Create a Cairo surface for drawing on the X11 window
        surface = cairo_xlib_surface_create(display, window, vinfo.visual, *width, *height);
        cr = cairo_create(surface);

        // Create the backbuffer Pixmap and Cairo surface
        backbuffer_pixmap = XCreatePixmap(display, window, *width, *height, DefaultDepth(display, screen));
        backbuffer_surface = cairo_xlib_surface_create(display, backbuffer_pixmap, DefaultVisual(display, screen), *width, *height);


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


void ClearScreen_x86(){
      // Flush drawing to the window    

    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE );
    cairo_paint(cr);      
}


void Render_x86( unsigned char* rgbaData, int u32Width, int u32Height){   
    
   // premultiplyAlpha((uint32_t*)rgbaData, u32Width, u32Height);

   //bitmap has been changed
    if (image_surface != NULL &&  cairo_image_surface_get_data(image_surface) != rgbaData){    
            cairo_surface_destroy(image_surface); 
            image_surface = NULL; 
    }
    // Create a Cairo image surface from the RGBA data
    if (image_surface==NULL){
        image_surface = cairo_image_surface_create_for_data(
        rgbaData, CAIRO_FORMAT_ARGB32, u32Width, u32Height, u32Width * 4);
    }else{                 
       // Update the Cairo surface data if needed (skip if the data is static)
        cairo_surface_mark_dirty(image_surface);
    }   
    cairo_set_source_surface(cr, image_surface, 1, 1);
    
    //cairo_set_operator(cr, CAIRO_OPERATOR_OVER);//Handles transparency and blends with the existing background, producing smooth visual results.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);//Faster, clears all below
    cairo_paint(cr);    
    
    //XSync(display, False);//seems not needed?
    
    //XRaiseWindow(display, window);//Raise the window to the top
}

void FlushDrawing_x86(){
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


extern uint16_t Transform_OVERLAY_WIDTH;
extern uint16_t Transform_OVERLAY_HEIGHT;
extern float Transform_Roll;
extern float Transform_Pitch;
bool outlined=true;
void drawLine_x86(int x0, int y0, int x1, int y1, uint32_t color, double thickness, bool Transpose) {
    
    if (Transpose){
         
        uint32_t width = Transform_OVERLAY_WIDTH;
        uint32_t height=Transform_OVERLAY_HEIGHT;
        // Apply Transform
        int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
        Point img_center = {Transform_OVERLAY_WIDTH / 2, Transform_OVERLAY_HEIGHT / 2};  // Center of the image

        // Define the four corners of the rectangle before rotation
        Point A = {x0, y0-OffsY};
        Point B = {x1, y1-OffsY};        

        // Rotate each corner around the center
        Point rotated_A, rotated_B;
        rotate_point(A, img_center, Transform_Roll, &rotated_A);
        rotate_point(B, img_center, Transform_Roll, &rotated_B);

        x0 = rotated_A.x;
        y0 = rotated_A.y;
        x1 = rotated_B.x;
        y1 = rotated_B.y;
        
    }

  // Extract the RGBA components from the color
    double r = ((color >> 24) & 0xFF) / 255.0;
    double g = ((color >> 16) & 0xFF) / 255.0;
    double b = ((color >> 8) & 0xFF) / 255.0;
    double a = 128;//(color & 0xFF) / 255.0;
    
    if (!outlined){
         cairo_set_source_rgba(cr, r, g, b, a); 
         cairo_set_line_width(cr, thickness);  
         cairo_move_to(cr, x0, y0);   
         cairo_line_to(cr, x1, y1);    
         cairo_stroke(cr); 
    }else {
        if (thickness>1)
            thickness--;

        //cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

        // Save the current state of the Cairo context
        cairo_save(cr);
        // Draw the outline (semi-transparent black) with a thicker line width
        cairo_set_source_rgba(cr, 0, 0, 0, 0.5);  // Semi-transparent black (alpha = 0.5)
        cairo_set_line_width(cr, thickness + 2);  // Make the outline slightly thicker than the main line
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
        cairo_stroke(cr);
        cairo_restore(cr);

        cairo_save(cr);
        // Draw the main line (colored) with the original thickness
        cairo_set_source_rgba(cr, r, g, b, a);  // Use the desired RGBA values for the line color
        cairo_set_line_width(cr, thickness);  
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
        cairo_stroke(cr);
        cairo_restore(cr);
    
    }
}

// Function to draw rotated text with transformation
void drawText_x86(const char* text, int x, int y, uint32_t color, double size, bool Transpose) {
    // Extract the RGBA components from the color
    double r = ((color >> 24) & 0xFF) / 255.0;
    double g = ((color >> 16) & 0xFF) / 255.0;
    double b = ((color >> 8) & 0xFF) / 255.0;
    double a = (color & 0xFF) / 255.0;

    // Set the color using RGBA values (each between 0.0 and 1.0)
    cairo_set_source_rgba(cr, r, g, b, a);

    if (Transpose) {
        uint32_t width = Transform_OVERLAY_WIDTH;
        uint32_t height = Transform_OVERLAY_HEIGHT;
        
        // Apply Transform
        int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
        Point img_center = {width / 2, height / 2}; // Center of the image

        // Define the position before rotation
        Point original_pos = {x, y - OffsY};
        Point rotated_pos;

        // Rotate the position around the center
        rotate_point(original_pos, img_center, Transform_Roll, &rotated_pos);

        // Update the transformed position
        x = rotated_pos.x;
        y = rotated_pos.y;
    }

    // Set the font size for the text
    cairo_set_font_size(cr, size);        

    if (false){
        cairo_move_to(cr, x, y);    
        cairo_show_text(cr, text);
    }else{
        // Save the current state of the Cairo context
        cairo_save(cr);        
        cairo_translate(cr, x, y);
    
        cairo_rotate(cr, Transform_Roll * (M_PI / 180.0));
        
        // This ensures that the text is drawn at the rotated position with (x, y) as the anchor
        cairo_move_to(cr, 0, 0);
        cairo_set_font_size(cr, size);        
        cairo_show_text(cr, text);
        // Restore the original state of the Cairo context (removes the rotation and translation)
        cairo_restore(cr);

    }
 
}