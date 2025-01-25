#include "../../bmp/bitmap.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/keysym.h>
#include <event2/event.h>

#if defined(_x86)
Display *display = NULL;
Window window;
Pixmap backbuffer_pixmap;
bool forcefullscreen = true;
extern struct event_base *base;
struct event *x11_event = NULL;
extern int AHI_TiltY;
Window RootWindow;
#endif

#if defined(__ROCKCHIP__)
#define SHM_NAME "msposd"

// Define the shared memory region structure
typedef struct {
	uint16_t width;		  // Image width
	uint16_t height;	  // Image height
	unsigned char data[]; // Flexible array for image data
} SharedMemoryRegion;
#endif

cairo_surface_t *surface = NULL;
cairo_surface_t *surface_back = NULL;
cairo_surface_t *image_surface = NULL;
cairo_t *cr = NULL;
cairo_t *cr_back = NULL;

extern bool verbose;

#if defined(_x86)
void rotate_point(Point original, Point img_center, double angle_degrees, Point *rotated);

void handle_key_press(XEvent *event) {
	KeySym keysym = XLookupKeysym(&event->xkey, 0); // Map keycode to keysym

	// Print the key that was pressed
	if (keysym == XK_Escape) {
		printf("Escape key pressed, exiting...\n");
		// exit(0);  // Exit the program on Escape key
	}
	if ((event->xkey.state & Mod1Mask) && (keysym == XK_Up)) {
		if (AHI_TiltY < 300)
			AHI_TiltY += 30;
		return;
	}
	if ((event->xkey.state & Mod1Mask) && (keysym == XK_Down)) {
		if (AHI_TiltY > -300)
			AHI_TiltY -= 30;
		return;
	}
}

void event_callback(evutil_socket_t fd, short event, void *arg) {
	XEvent xevent;
	while (XPending(display)) { // Process all queued X events
		XNextEvent(display, &xevent);
		if (xevent.type == Expose) {
			// Do something here
		} else if (xevent.type == KeyPress) {
			handle_key_press(&xevent);
		}
	}
}

int Init(uint16_t *width, uint16_t *height) {
	if (verbose)
		forcefullscreen = false;

	setenv("DISPLAY", ":0", 0);

	display = XOpenDisplay(NULL);
	if (!display) {
		fprintf(stderr, "Cannot open display\n");
		return 1;
	}
	int screen = DefaultScreen(display);
	RootWindow = RootWindow(display, screen);

	uint16_t screen_width = DisplayWidth(display, screen);
	uint16_t screen_height = DisplayHeight(display, screen);
	*width = screen_width;
	*height = screen_height;
	// Set up an ARGB visual for transparency
	XVisualInfo vinfo;
	if (!XMatchVisualInfo(display, screen, 32, TrueColor, &vinfo)) {
		fprintf(stderr, "No 32-bit visual found\n");
		return 1;
	}

	XSetWindowAttributes attrs;
	attrs.colormap = XCreateColormap(display, RootWindow, vinfo.visual, AllocNone);
	attrs.border_pixel = 0;
	attrs.background_pixel = 0;
	//!!!!!!!!!!!!!!!!!!!!!!!!       THIS Removes window's borders
	attrs.override_redirect = True; // Make the window borderless, MAKE TRUE
	if (!forcefullscreen)			// debug mode
		attrs.override_redirect = False;

	// Create the window with transparency support
	window = XCreateWindow(display, RootWindow, 0, 0, *width, *height, 0, vinfo.depth, InputOutput,
		vinfo.visual, CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect, &attrs);

	// Set the window name (title)
	// const char *window_title = "MSPOSD";
	// XStoreName(display, window, window_title);

	// Select input events: expose, key press, key release
	XSelectInput(display, window, ExposureMask | KeyPressMask | KeyReleaseMask);

	char buffer[50]; // tried to set env var to read it from python, didn't work
					 // ....
	snprintf(buffer, sizeof(buffer), "%lu", window);
	setenv("MSP_WINDOW_ID", buffer,
		1); // 1 means overwrite if it already exists

	// Make the window topmost?
	XMapWindow(display, window);

	// Raise the window to the top, may slow down and interfere with the UI,
	// disable for debugging
	XRaiseWindow(display, window);

	// Set input focus to the window
	// XSetInputFocus(display, window, RevertToParent, CurrentTime);

	// Set window properties to make it transparent
	if (false) { // seems not needed
		Atom windowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
		Atom windowTypeDock = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
		XChangeProperty(display, window, windowType, XA_ATOM, 32, PropModeReplace,
			(unsigned char *)&windowTypeDock, 1);
	}
	// Create a Cairo surface for drawing on the X11 window
	surface_back = cairo_xlib_surface_create(display, window, vinfo.visual, *width, *height);
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, *width, *height);
	cr = cairo_create(surface);
	cr_back = cairo_create(surface_back);
}
#endif

#if defined(__ROCKCHIP__)
int Init(uint16_t *width, uint16_t *height) {
	// Create shared memory region
	const char *shm_name = "msposd"; // Name of the shared memory region
	int shm_fd = -1;

	// Wait until the shared memory segment exists
	while (shm_fd == -1) {
		shm_fd = shm_open(shm_name, O_RDWR, 0666);
		if (shm_fd == -1) {
			if (errno == ENOENT) {
				printf("Shared memory '%s' does not exist. Waiting...\n", shm_name);
				sleep(1); // Wait for 1 second before trying again
				continue;
			} else {
				perror("Failed to open shared memory");
				return -1;
			}
		}
	}
	// Map just the header to read width and height
	size_t header_size = sizeof(SharedMemoryRegion);
	SharedMemoryRegion *shm_region =
		(SharedMemoryRegion *)mmap(0, header_size, PROT_READ, MAP_SHARED, shm_fd, 0);
	if (shm_region == MAP_FAILED) {
		perror("Failed to map shared memory header");
		close(shm_fd);
		return -1;
	}

	// Validate width and height (optional)
	if (shm_region->width <= 0 || shm_region->width <= 0) {
		fprintf(stderr, "Invalid width or height in shared memory\n");
		munmap(shm_region, header_size);
		close(shm_fd);
		return -1;
	}

	int shm_width = shm_region->width;
	int shm_height = shm_region->height;
	*width = shm_region->width;
	*height = shm_region->height;

	printf("Surface is %ix%i pixels\n", shm_width, shm_height);

	// Unmap the header (optional, as we'll remap everything)
	munmap(shm_region, header_size);

	// Calculate the total size of shared memory
	size_t shm_size = header_size + (shm_width * shm_height * 4); // Header + Image data

	// Remap the entire shared memory region (header + image data)
	shm_region =
		(SharedMemoryRegion *)mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (shm_region == MAP_FAILED) {
		perror("Failed to remap shared memory");
		close(shm_fd);
		return -1;
	}

	// Create a Cairo surface for the image data
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, shm_width, shm_width);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "Failed to create Cairo surface\n");
		return -1;
	}

	// Create a Cairo surface for the image data connecto to the shm
	surface_back = cairo_image_surface_create_for_data(
		shm_region->data, CAIRO_FORMAT_ARGB32, shm_width, shm_height, shm_width * 4);
	if (cairo_surface_status(surface_back) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "Failed to create Cairo surface_back\n");
		munmap(shm_region, shm_size);
		close(shm_fd);
		return -1;
	}

	// Create a Cairo context
	cr = cairo_create(surface);
	cr_back = cairo_create(surface_back);
}
#endif

void premultiplyAlpha(uint32_t *rgbaData, uint32_t width, uint32_t height) {
	for (uint32_t i = 0; i < width * height; ++i) {
		uint8_t *pixel = (uint8_t *)&rgbaData[i];
		uint8_t a = pixel[3]; // Alpha channel

		// Premultiply RGB by alpha
		pixel[0] = (pixel[0] * a) / 255; // Blue channel
		pixel[1] = (pixel[1] * a) / 255; // Green channel
		pixel[2] = (pixel[2] * a) / 255; // Red channel
	}
}

void ClearScreen() {
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
}

void Render(unsigned char *rgbaData, int u32Width, int u32Height) {

	cairo_set_source_rgba(cr, 0, 0, 0, 0); // Transparent background for buffer
	cairo_set_operator(cr,
		CAIRO_OPERATOR_SOURCE); // Clear everything on the buffer
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); // Restore default operator

	// bitmap has been changed
	if (image_surface != NULL && cairo_image_surface_get_data(image_surface) != rgbaData) {
		cairo_surface_destroy(image_surface);
		image_surface = NULL;
	}
	// Create a Cairo image surface from the RGBA data
	if (image_surface == NULL) {
		image_surface = cairo_image_surface_create_for_data(
			rgbaData, CAIRO_FORMAT_ARGB32, u32Width, u32Height, u32Width * 4);
	} else {
		cairo_surface_mark_dirty(image_surface);
	}
	cairo_set_source_surface(cr, image_surface, 1, 1);

	// cairo_set_operator(cr, CAIRO_OPERATOR_OVER);//Handles transparency and
	// blends with the existing background, producing smooth visual results.
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); // Faster, clears all below
	cairo_paint(cr);
	// XSync(display, False);//seems not needed?

#if defined(_x86)
	if (forcefullscreen)
		XRaiseWindow(display, window); // Raise the window to the top
#endif
}

void Render_rect(unsigned char *rgbaData, int u32Width, int u32Height, int src_x, int src_y,
	int dest_x, int dest_y, int rect_width, int rect_height) {
	// Check if the bitmap has changed and recreate the image surface if
	// necessary
	if (image_surface != NULL && cairo_image_surface_get_data(image_surface) != rgbaData) {
		cairo_surface_destroy(image_surface);
		image_surface = NULL;
	}

	// Create a Cairo image surface from the RGBA data if it hasn't been created
	// yet
	if (image_surface == NULL) {
		image_surface = cairo_image_surface_create_for_data(
			rgbaData, CAIRO_FORMAT_ARGB32, u32Width, u32Height, u32Width * 4);
	} else {
		// Mark the image surface as dirty if the RGBA data has changed
		cairo_surface_mark_dirty(image_surface);
	}

	// Define the clipping area to copy only a portion of the source image
	cairo_rectangle(cr, dest_x, dest_y, rect_width, rect_height);
	cairo_clip(cr);

	// Set the source surface, offset by src_x and src_y to start drawing from
	// the specified portion
	cairo_set_source_surface(cr, image_surface, dest_x - src_x, dest_y - src_y);
	cairo_set_operator(cr,
		/*CAIRO_OPERATOR_SOURCE*/ CAIRO_OPERATOR_OVER); // Use SOURCE to copy
														// without blending
	cairo_paint(cr);

	// Reset the clip to allow future drawings without being constrained
	cairo_reset_clip(cr);
}

void FlushDrawing() {
#if defined(_x86)
	// Hook keyboard- can't be in init procs since there the libevent is not
	// still created.
	//  base = event_base_new();
	if (x11_event == NULL && base != NULL) {
		// Attach X11 display's file descriptor to the existing msposd
		// event_base
		struct event *x11_event =
			event_new(base, ConnectionNumber(display), EV_READ | EV_PERSIST, event_callback, NULL);
		event_add(x11_event, NULL);

		// if (XGrabKeyboard(display, window, True, GrabModeAsync,
		// GrabModeAsync, CurrentTime) != GrabSuccess)
		//    fprintf(stderr, "Failed to grab keyboard\n");
		// Grab only the specific key combinations
		// XGrabKey(display, XKeysymToKeycode(display, XK_Up), Mod1Mask, window,
		// True,
		//      GrabModeAsync, GrabModeAsync); // Alt + Up Arrow
		// XGrabKey(display, XKeysymToKeycode(display, XK_Down), Mod1Mask,
		// window, True,
		//      GrabModeAsync, GrabModeAsync); // Alt + Up Arrow
		// XGrabKey(display, XKeysymToKeycode(display, XK_Left), ShiftMask,
		// window, True,
		//      GrabModeAsync, GrabModeAsync); // Shift + Left Arrow

		XGrabKey(display, XKeysymToKeycode(display, XK_Up), Mod1Mask, RootWindow, True,
			GrabModeAsync,
			GrabModeAsync); // Alt + Up Arrow
		XGrabKey(display, XKeysymToKeycode(display, XK_Down), Mod1Mask, RootWindow, True,
			GrabModeAsync,
			GrabModeAsync); // Alt + Down Arrow
	}
#endif

	// Copy work buffer to the display surface do avoid flickering
	cairo_set_operator(cr_back, CAIRO_OPERATOR_SOURCE);
	// Copy buffer to the display surface
	cairo_set_source_surface(cr_back, surface, 0, 0);
	cairo_paint(cr_back);

	cairo_surface_flush(surface_back);
#if defined(_x86)
	XFlush(display);
#endif
	// XSync(display, False);//seems not needed?
}

void Close() {
	// Clean up resources
	cairo_destroy(cr);
	cairo_destroy(cr_back);
	cairo_surface_destroy(image_surface);
	cairo_surface_destroy(surface);
	cairo_surface_destroy(surface_back);
#if defined(_x86)
	XDestroyWindow(display, window);
	XCloseDisplay(display);

	// Cleanup
	event_free(x11_event);
	event_base_free(base);
#endif
}

extern uint16_t Transform_OVERLAY_WIDTH;
extern uint16_t Transform_OVERLAY_HEIGHT;
extern float Transform_Roll;
extern float Transform_Pitch;
bool outlined = true;

void drawLineGS(int x0, int y0, int x1, int y1, uint32_t color, double thickness, bool Transpose) {

	if (Transpose) {

		uint32_t width = Transform_OVERLAY_WIDTH;
		uint32_t height = Transform_OVERLAY_HEIGHT;
		// Apply Transform
		int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
		Point img_center = {
			Transform_OVERLAY_WIDTH / 2, Transform_OVERLAY_HEIGHT / 2}; // Center of the image

		// Define the four corners of the rectangle before rotation
		Point A = {x0, y0 - OffsY};
		Point B = {x1, y1 - OffsY};

		// Rotate each corner around the center
		Point rotated_A, rotated_B;
		rotate_point(A, img_center, Transform_Roll, &rotated_A);
		rotate_point(B, img_center, Transform_Roll, &rotated_B);

		x0 = rotated_A.x;
		y0 = rotated_A.y;
		x1 = rotated_B.x;
		y1 = rotated_B.y;
	}

	double r = ((color >> 24) & 0xFF) / 255.0;
	double g = ((color >> 16) & 0xFF) / 255.0;
	double b = ((color >> 8) & 0xFF) / 255.0;
	double a = (color & 0xFF) / 255.0; // 128

	if (!outlined) {
		cairo_set_source_rgba(cr, r, g, b, a);
		cairo_set_line_width(cr, thickness);
		cairo_move_to(cr, x0, y0);
		cairo_line_to(cr, x1, y1);
		cairo_stroke(cr);
	} else {
		if (thickness > 1)
			thickness--;

		// we can turn off
		// cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

		// Save the current state of the Cairo context
		cairo_save(cr);
		// Draw the outline (semi-transparent black) with a thicker line width
		cairo_set_source_rgba(cr, 0, 0, 0,
			0.5); // Semi-transparent black (alpha = 0.5)
		cairo_set_line_width(cr,
			thickness + 2); // Make the outline slightly thicker than the main line
		cairo_move_to(cr, x0, y0);
		cairo_line_to(cr, x1, y1);

		cairo_stroke(cr);
		cairo_restore(cr);

		cairo_save(cr);
		// Draw the main line (colored) with the original thickness
		cairo_set_source_rgba(cr, r, g, b,
			a); // Use the desired RGBA values for the line color
		cairo_set_line_width(cr, thickness);
		cairo_move_to(cr, x0, y0);
		cairo_line_to(cr, x1, y1);
		cairo_stroke(cr);
		cairo_restore(cr);
	}
}

// Function to draw rotated text with rotation
int getTextWidth(const char *text, double size) {
	cairo_set_font_size(cr, size);
	cairo_text_extents_t extents;
	// Get the extents of the text
	cairo_text_extents(cr, text, &extents);
	// Return the width of the text
	return (int)extents.width;
}

// Function to draw rotated text with rotation
void drawText(
	const char *text, int x, int y, uint32_t color, double size, bool Transpose, int Outline) {
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

		Point original_pos = {x, y - OffsY};
		Point rotated_pos;

		// Rotate the position around the center
		rotate_point(original_pos, img_center, Transform_Roll, &rotated_pos);

		// Update the transformed position
		x = rotated_pos.x;
		y = rotated_pos.y;
	}

	cairo_set_font_size(cr, size);

	if (false) {
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, text);
	} else {
		// Save the current state of the Cairo context
		cairo_save(cr);
		cairo_translate(cr, x, y);

		if (Transpose)
			cairo_rotate(cr, Transform_Roll * (M_PI / 180.0));

		if (Outline) {
			// Draw the outline (stroke) first with a larger line width
			cairo_set_line_width(cr, Outline); // Adjust the width as needed for the outline
			cairo_set_source_rgba(cr, 0, 0, 0,
				1.0); // Set color for the outline (black)
			cairo_move_to(cr, 0, 0);
			cairo_text_path(cr, text); // Create a path for the text
			cairo_stroke(cr);		   // Stroke the path (outline)
		}
		// Draw the text itself (fill) on top of the outline
		cairo_set_line_width(cr, 1.0); // Reset line width to normal
		cairo_set_source_rgba(cr, r, g, b,
			a); // Set the original color for the text
		// Fill the text path with the text color
		cairo_move_to(cr, 0, 0);
		cairo_text_path(cr, text); // Create the path for the text
		cairo_fill(cr);			   // Fill the path with the text color

		// cairo_move_to(cr, 0, 0);
		// cairo_set_font_size(cr, size);
		// cairo_show_text(cr, text);

		cairo_restore(cr);
	}
}

void draw_rounded_rectangle(
	cairo_t *cr, double x, double y, double width, double height, double radius) {
	cairo_move_to(cr, x + radius, y);
	cairo_arc(cr, x + width - radius, y + radius, radius, -90 * (M_PI / 180),
		0 * (M_PI / 180)); // Top-right corner
	cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * (M_PI / 180),
		90 * (M_PI / 180)); // Bottom-right corner
	cairo_arc(cr, x + radius, y + height - radius, radius, 90 * (M_PI / 180),
		180 * (M_PI / 180)); // Bottom-left corner
	cairo_arc(cr, x + radius, y + radius, radius, 180 * (M_PI / 180),
		270 * (M_PI / 180)); // Top-left corner
	cairo_close_path(cr);
}

void drawRC_Channels(int posX, int posY, int m1X, int m1Y, int m2X,
	int m2Y) { // int pitch, int roll, int throttle, int yaw
	int width = 100, height = 100;

	int side = 64;

	// Draw the outline of the rounded rectangle
	cairo_set_source_rgb(cr, 1, 1, 1); // White color for outline
	cairo_set_line_width(cr, 1);	   // Line thickness
	draw_rounded_rectangle(cr, posX, posY, side, side,
		16); // 72x72 px rectangle with 10 px corner radius
	cairo_stroke(cr);
	// Draw a small circle at the center of the rounded rectangle
	double circle_diameter = 8;
	double circle_radius = circle_diameter / 2;
	double rect_center_x = posX + side * (m2X - 1000) / 1000;
	double rect_center_y = posY + side - side * (m2Y - 1000) / 1000;
	cairo_set_source_rgb(cr, 1, 1, 1); // Set color to white for the circle
	cairo_arc(cr, rect_center_x, rect_center_y, circle_radius, 0, 2 * M_PI);
	cairo_fill(cr);

	// Draw next stick
	cairo_set_source_rgb(cr, 1, 1, 1); // White color for outline
	cairo_set_line_width(cr, 1);	   // Line thickness
	draw_rounded_rectangle(cr, posX + side + 5, posY, side, side,
		16); // 72x72 px rectangle with 10 px corner radius
	cairo_stroke(cr);

	// Draw a small circle at the center of the rounded rectangle
	rect_center_x = posX + side + 5 + side * (m1X - 1000) / 1000;
	rect_center_y = posY + side - side * (m1Y - 1000) / 1000;
	cairo_set_source_rgb(cr, 1, 1, 1); // Set color to white for the circle
	cairo_arc(cr, rect_center_x, rect_center_y, circle_radius, 0, 2 * M_PI);
	cairo_fill(cr);
}
