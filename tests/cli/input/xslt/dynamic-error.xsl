<?xml version="1.0"?>
<!-- A dynamic error: the report gives the line of the expression. -->
<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:template name="xsl:initial-template">
    <xsl:value-of select="count(doc('../books.xml')//book) idiv 0"/>
  </xsl:template>
</xsl:stylesheet>
