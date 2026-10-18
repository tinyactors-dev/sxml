<?xml version="1.0"?>
<!-- The initial mode chooses the template rules; xsl:result-document
     without href is the principal result. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:mode name="titles" on-no-match="shallow-skip"/>
  <xsl:template match="/">
    <xsl:result-document method="html">
      <html><body><p>all: <xsl:value-of select="count(//book)"/></p></body></html>
    </xsl:result-document>
  </xsl:template>
  <xsl:template match="title" mode="titles">
    <xsl:value-of select="."/><xsl:text>;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
