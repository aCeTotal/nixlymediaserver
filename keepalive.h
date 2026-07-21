/*
 * Nixly Media Server - Disk keepalive
 *
 * Holder media-diskene våkne så lenge en klient nylig har streamet, så
 * spindown-HDD-er ikke spinner ned i pauser / mellom episoder / under bla i
 * biblioteket. Uten dette staller neste read (resume, seek langt inn) flere
 * sekunder mens platen spinner opp — klienten ser frys / svart skjerm.
 */

#ifndef KEEPALIVE_H
#define KEEPALIVE_H

/* Start bakgrunnstråden. Kall én gang etter at config og DB er lastet. */
void keepalive_start(void);

/* Marker klient-media-aktivitet (kalles per /stream-request). Holder diskene
 * våkne et vindu etterpå. */
void keepalive_notify_activity(void);

#endif /* KEEPALIVE_H */
