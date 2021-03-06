# This scenario is designed to reach the `case EHA_INDEXED_DYN`
# in the `lsqpack_enc_encode` function. It is intended to fail the integer
# encoding due to buffer length. To trigger this case, many names and values
# must be inserted into the dynamic table so that the integer encoding requires
# more than one byte.
TABLE_SIZE=16384
AGGRESSIVE=1
IMMEDIATE_ACK=0
RISKED_STREAMS=1000
QIF=$(cat<<'EOQ'
aaaaaaaaaaaaa	aaaaaaaaaaaaaaaaaaaaaaaaaa
bbbbbbbbbbbbb	bbbbbbbbbbbbbbbbbbbbbbbbbb
ccccccccccccc	cccccccccccccccccccccccccc
ddddddddddddd	dddddddddddddddddddddddddd
eeeeeeeeeeeee	eeeeeeeeeeeeeeeeeeeeeeeeee
fffffffffffff	ffffffffffffffffffffffffff
ggggggggggggg	gggggggggggggggggggggggggg
aaaa	oneeeeeeeee
bbbb	oneeeeeeeee
cccc	oneeeeeeeee
dddd	oneeeeeeeee
eeee	oneeeeeeeee
ffff	oneeeeeeeee
gggg	oneeeeeeeee
hhhh	oneeeeeeeee
iiii	oneeeeeeeee
jjjj	oneeeeeeeee
kkkk	oneeeeeeeee
llll	oneeeeeeeee
mmmm	oneeeeeeeee
nnnn	oneeeeeeeee
oooo	oneeeeeeeee
pppp	oneeeeeeeee
qqqq	oneeeeeeeee
rrrr	oneeeeeeeee
ssss	oneeeeeeeee
tttt	oneeeeeeeee
uuuu	oneeeeeeeee
vvvv	oneeeeeeeee
wwww	oneeeeeeeee
xxxx	oneeeeeeeee
yyyy	oneeeeeeeee
zzzz	oneeeeeeeee
aabb	oneeeeeeeee
bbcc	oneeeeeeeee
ccdd	oneeeeeeeee
ddee	oneeeeeeeee
eeff	oneeeeeeeee
ffgg	oneeeeeeeee
gghh	oneeeeeeeee
hhii	oneeeeeeeee
iijj	oneeeeeeeee
jjkk	oneeeeeeeee
kkll	oneeeeeeeee
llmm	oneeeeeeeee
mmnn	oneeeeeeeee
nnoo	oneeeeeeeee
oopp	oneeeeeeeee
ppqq	oneeeeeeeee
qqrr	oneeeeeeeee
rrss	oneeeeeeeee
sstt	oneeeeeeeee
ttuu	oneeeeeeeee
uuvv	oneeeeeeeee
vvww	oneeeeeeeee
wwxx	oneeeeeeeee
xxyy	oneeeeeeeee
yyzz	oneeeeeeeee
abcd	oneeeeeeeee
bcde	oneeeeeeeee
cdef	oneeeeeeeee
defg	oneeeeeeeee
efgh	oneeeeeeeee
fghi	oneeeeeeeee
ghij	oneeeeeeeee
hijk	oneeeeeeeee
ijkl	oneeeeeeeee
jklm	oneeeeeeeee
klmn	oneeeeeeeee
lmno	oneeeeeeeee
mnop	oneeeeeeeee
nopq	oneeeeeeeee
opqr	oneeeeeeeee
pqrs	oneeeeeeeee
qrst	oneeeeeeeee
rstu	oneeeeeeeee

aaaa	oneeeeeeeee
bbbb	oneeeeeeeee
cccc	oneeeeeeeee
dddd	oneeeeeeeee
eeee	oneeeeeeeee
ffff	oneeeeeeeee
gggg	oneeeeeeeee
hhhh	oneeeeeeeee
iiii	oneeeeeeeee
jjjj	oneeeeeeeee
kkkk	oneeeeeeeee
llll	oneeeeeeeee
mmmm	oneeeeeeeee
nnnn	oneeeeeeeee
oooo	oneeeeeeeee
pppp	oneeeeeeeee
qqqq	oneeeeeeeee
rrrr	oneeeeeeeee
ssss	oneeeeeeeee
tttt	oneeeeeeeee
uuuu	oneeeeeeeee
vvvv	oneeeeeeeee
wwww	oneeeeeeeee
xxxx	oneeeeeeeee
yyyy	oneeeeeeeee
zzzz	oneeeeeeeee
aabb	oneeeeeeeee
bbcc	oneeeeeeeee
ccdd	oneeeeeeeee
ddee	oneeeeeeeee
eeff	oneeeeeeeee
ffgg	oneeeeeeeee
gghh	oneeeeeeeee
hhii	oneeeeeeeee
iijj	oneeeeeeeee
jjkk	oneeeeeeeee
kkll	oneeeeeeeee
llmm	oneeeeeeeee
mmnn	oneeeeeeeee
nnoo	oneeeeeeeee
oopp	oneeeeeeeee
ppqq	oneeeeeeeee
qqrr	oneeeeeeeee
rrss	oneeeeeeeee
sstt	oneeeeeeeee
ttuu	oneeeeeeeee
uuvv	oneeeeeeeee
vvww	oneeeeeeeee
wwxx	oneeeeeeeee
xxyy	oneeeeeeeee
yyzz	oneeeeeeeee
aaaa	oneeeeeeeee
abcd	oneeeeeeeee
bcde	oneeeeeeeee
cdef	oneeeeeeeee
defg	oneeeeeeeee
efgh	oneeeeeeeee
fghi	oneeeeeeeee
ghij	oneeeeeeeee
hijk	oneeeeeeeee
ijkl	oneeeeeeeee
jklm	oneeeeeeeee
klmn	oneeeeeeeee
lmno	oneeeeeeeee
mnop	oneeeeeeeee
nopq	oneeeeeeeee
opqr	oneeeeeeeee
pqrs	oneeeeeeeee
qrst	oneeeeeeeee
rstu	oneeeeeeeee
aaaa	oneeeeeeeee

EOQ
)
