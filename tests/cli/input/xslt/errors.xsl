<?xml version="1.0"?>
<!-- Static errors in several declarations: each is reported on a line of
     its own (D315, D320). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:template match="item[">
    <xsl:value-of select="."/>
  </xsl:template>
  <xsl:template match="book" bogus="1">
    <xsl:value-of select="1 +"/>
  </xsl:template>
</xsl:stylesheet>
