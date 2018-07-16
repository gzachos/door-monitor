#!/usr/bin/env bash

# Modify at your discretion!

TO='username@domain.xz'
FROM='RasPi Root<username@domain.yz>'

SUBJECT="Door $1 - `date +%H%M%S`"
CONTENT="Your home's main entrance has $1!\nDate: `date --date="@$2"`"
CONTENT_FILE="`mktemp mail.XXXXX`"

echo -e "${CONTENT}" > ${CONTENT_FILE}

mail	-s "[CSE RPi] ${SUBJECT}" \
	-t "${TO}" \
	-a Importance:High \
	-a From:"${FROM}" \
	< ${CONTENT_FILE}
RETVAL=$?

rm ${CONTENT_FILE}

exit ${RETVAL}

