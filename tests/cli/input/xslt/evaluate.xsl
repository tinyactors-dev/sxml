<?xml version="1.0"?>
<!-- xsl:evaluate (XSLT 4.0, 10.5): the expression comes from the command line
     and runs with the document as its context item and $tax as a parameter. -->
<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:param name="expression" select="'count(//book)'"/>
  <xsl:output method="text"/>
  <xsl:template match="/">
    <xsl:evaluate xpath="$expression" context-item=".">
      <xsl:with-param name="tax" select="0.25"/>
    </xsl:evaluate>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
