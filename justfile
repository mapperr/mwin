_default:
	just --list

make *ARGS:
	#!/bin/sh
	xxchroot run dev bash -c "cd $PWD && make {{ARGS}}"
