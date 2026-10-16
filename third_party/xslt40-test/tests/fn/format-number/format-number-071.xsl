<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:xs="http://www.w3.org/2001/XMLSchema"
  xmlns:mml="http://www.w3.org/1998/Math/MathML"
  xmlns:c="Crane"
  exclude-result-prefixes="xs c"
  version="3.0">
  
  <xsl:decimal-format name="c:SI" grouping-separator="&#xA0;"/>
  
  <xsl:output method="text"/>
  
  <xsl:template name="xsl:initial-template">
    <out result="{format-number(12345.6789012345, '##,##.###,###,###')}"/>
  </xsl:template>
  
</xsl:stylesheet>