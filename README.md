# wtw

wtw is a simple text widget for wlroot compositors; ported from
[stw](https://github.com/sineemore/stw), with wtw inheriting it's behavior.

![](example.png)

## Building

To build wtw first ensure that you have the following dependencies:

* wayland
* wayland-protocols
* fcft
* pixman
* pkg-config

Afterwards, run:
```
make
make install
```

## Usage
When using wtw, ensure you guard the command-line arguments with '--'.

Example usage:
```
wtw -b 181716aa -c ebdbb2ff -P 10 -x 20 -y 20 -- pstree -U
```
