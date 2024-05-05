#include <tigr.h>

int main() {
	Tigr *tigr = tigrWindow(320, 240, "term-emu", 0);
	while(!tigrClosed(tigr) && !tigrKeyDown(tigr, TK_ESCAPE)) {
		tigrClear(tigr, tigrRGB(24, 24, 24));
		tigrPrint(tigr, tfont, 120, 110, tigrRGB(240, 240, 240), "It Works!");
		tigrUpdate(tigr);
	}

	tigrFree(tigr);
	return 0;
}
